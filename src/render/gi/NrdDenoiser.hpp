#pragma once

#include "render/FrameGraph.hpp"
#include "render/core/FrameIdentity.hpp"
#include "render/core/History.hpp"

#include <NRD.h>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lumin::render::gi {

    /**
     * @brief 将一张应用侧纹理同时关联到物理资源与现有 FrameGraph 身份。
     *
     * NRD adapter 不会再次导入应用侧信号。`graphResource` 必须正是创建 `texture` 当前内容的
     * FrameGraph 资源，借此让 ray tracing、NRD 与后续合成共享同一条依赖链。
     */
    struct NrdTextureBinding {
        nvrhi::ITexture* texture = nullptr;
        FrameGraphResourceHandle graphResource;
    };

    /** REBLUR_DIFFUSE_SPECULAR 每帧需要的五类输入信号。 */
    struct NrdSignalBindings {
        /// 已按 REBLUR 前端契约编码的 diffuse radiance 与 normalized hit distance。
        NrdTextureBinding diffuseRadianceHitDistance;
        /// 已按 REBLUR 前端契约编码的 specular radiance 与 normalized hit distance。
        NrdTextureBinding specularRadianceHitDistance;
        /// 线性 view-space Z，天空像素应不小于 `denoisingRange`。
        NrdTextureBinding viewZ;
        /// 按当前 NRD library normal/roughness encoding 打包的数据。
        NrdTextureBinding normalRoughness;
        /// 非抖动 screen-UV motion，方向固定为 `previous - current`。
        NrdTextureBinding motion;
    };

    /** adapter 拥有的两张持久化 REBLUR 输出纹理。 */
    struct NrdOutputResources {
        nvrhi::TextureHandle diffuseRadianceHitDistance;
        nvrhi::TextureHandle specularRadianceHitDistance;
    };

    /** 当前 FrameGraph 中可供后续 GI composite 读取的 NRD 输出。 */
    struct NrdGraphOutputs {
        FrameGraphResourceHandle diffuseRadianceHitDistance;
        FrameGraphResourceHandle specularRadianceHitDistance;
    };

    /**
     * @brief NRD 所需的非抖动相机矩阵与像素抖动。
     *
     * 四个矩阵均按 column-major 连续存储，并采用 column vector 乘法。投影矩阵不得包含 TAA jitter；
     * `jitter`/`jitterPrevious` 单位为像素，两个分量都必须位于 `[-0.5, 0.5]`。
     */
    struct NrdCameraData {
        std::array<float, 16> viewToClip = {
            1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
        };
        std::array<float, 16> viewToClipPrevious = viewToClip;
        std::array<float, 16> worldToView = viewToClip;
        std::array<float, 16> worldToViewPrevious = viewToClip;
        std::array<float, 2> jitter{};
        std::array<float, 2> jitterPrevious{};
    };

    /** 一次 REBLUR 录制尝试的稳定输入。 */
    struct NrdFrameParameters {
        core::FrameSlotIndex frameSlot;
        core::RenderSequence sequence;
        core::RenderExtent extent;
        NrdCameraData camera;
        core::HistoryAction diffuseHistory = core::HistoryAction::Keep;
        core::HistoryAction specularHistory = core::HistoryAction::Keep;
        /// 即使上层历史策略遗漏该原因，显式切镜也必须重启 REBLUR。
        bool cameraCut = false;
        /// 即使新旧尺寸数值偶然一致，资源重建也必须重启 REBLUR。
        bool renderResourcesRecreated = false;
        /// `true` 表示 `frameSlot` 对应 fence 已在本次 CPU 写入前等待完成。
        bool frameSlotFenceWaited = false;
        /// 两帧之间的毫秒数；零表示让 NRD 使用内部计时。
        float timeDeltaMilliseconds = 0.0F;
        /// 超出该线性 view Z 的像素不参与去噪。
        float denoisingRange = 500000.0F;
    };

    /** 描述一个 NRD 历史域最近一次成功提交后的状态。 */
    struct NrdHistoryState {
        bool valid = false;
        std::uint64_t resetEpoch = 0;
        std::uint64_t acceptedFrameCount = 0;
        core::HistoryAction lastCommittedAction = core::HistoryAction::Keep;
        core::RenderSequence lastSuccessfulSequence;

        friend constexpr bool operator==(const NrdHistoryState&, const NrdHistoryState&) noexcept = default;
    };

    /**
     * @brief 一次尚未提交的双域历史计划。
     *
     * `REBLUR_DIFFUSE_SPECULAR` 只有一个 `CommonSettings::accumulationMode`。因此任一域要求重启时，
     * diffuse 与 specular 都会执行同一次物理重启，`effective*Action` 会如实反映这一点。
     */
    struct NrdHistoryPlan {
        core::RenderSequence sequence;
        std::uint32_t frameIndex = 0;
        nrd::AccumulationMode accumulationMode = nrd::AccumulationMode::CONTINUE;
        core::HistoryAction requestedDiffuseAction = core::HistoryAction::Keep;
        core::HistoryAction requestedSpecularAction = core::HistoryAction::Keep;
        core::HistoryAction effectiveDiffuseAction = core::HistoryAction::Keep;
        core::HistoryAction effectiveSpecularAction = core::HistoryAction::Keep;
    };

    /**
     * @brief 维护 NRD diffuse/specular 的两阶段提交状态。
     *
     * `beginFrame()` 只创建候选计划。只有 `commitSubmittedFrame()` 会增加 NRD frame index、发布历史有效性
     * 和 reset epoch；`discardFrame()` 保留全部已提交状态，因此同一逻辑帧可以安全重试。
     */
    class NrdHistoryTracker final {
    public:
        [[nodiscard]] NrdHistoryPlan beginFrame(core::RenderSequence sequence, core::HistoryAction diffuseAction,
                                                core::HistoryAction specularAction, bool cameraCut,
                                                bool resourcesRecreated);
        void commitSubmittedFrame(core::RenderSequence sequence);
        void discardFrame(core::RenderSequence sequence);

        [[nodiscard]] bool hasActiveFrame() const noexcept;
        [[nodiscard]] const NrdHistoryState& state(core::HistoryDomain domain) const;

    private:
        void requireActive(core::RenderSequence sequence) const;

        std::array<NrdHistoryState, 2> states_{};
        std::unique_ptr<NrdHistoryPlan> activePlan_;
        std::uint64_t committedFrameCount_ = 0;
    };

    struct NrdDenoiserCreateInfo {
        nvrhi::IDevice* device = nullptr;
        core::RenderExtent extent;
        std::uint32_t frameSlotCount = 0;
        nrd::ReblurSettings reblurSettings{};
    };

    /**
     * @brief `record()` 返回的提交票据。
     *
     * 票据只标识一次候选录制，不拥有 adapter。调用方必须在提交结果已知后恰好调用一次
     * `commitSubmittedFrame()`、`discardFrame()` 或 adapter 的 `discardPendingFrame()`，并且不得把旧票据用于
     * 同序号的重试。
     */
    class NrdPreparedFrame final {
    public:
        NrdPreparedFrame() noexcept = default;

        [[nodiscard]] bool isValid() const noexcept;
        [[nodiscard]] core::RenderSequence sequence() const noexcept;
        [[nodiscard]] core::FrameSlotIndex frameSlot() const noexcept;
        [[nodiscard]] const NrdHistoryPlan& historyPlan() const;
        [[nodiscard]] const NrdGraphOutputs& outputs() const noexcept;
        [[nodiscard]] std::span<const FrameGraphPassHandle> passes() const noexcept;

    private:
        friend class NrdDenoiser;

        std::uint64_t token_ = 0;
        core::RenderSequence sequence_;
        core::FrameSlotIndex frameSlot_;
        NrdHistoryPlan historyPlan_;
        NrdGraphOutputs outputs_;
        std::vector<FrameGraphPassHandle> passes_;
    };

    namespace detail {

        enum class NrdPoolKind : std::uint8_t {
            Permanent,
            Transient,
        };

        struct NrdPoolTexturePlan {
            NrdPoolKind pool = NrdPoolKind::Permanent;
            std::uint16_t index = 0;
            nvrhi::TextureDesc texture;
        };

        enum class NrdResourceSource : std::uint8_t {
            User,
            PermanentPool,
            TransientPool,
        };

        struct NrdResourceReference {
            NrdResourceSource source = NrdResourceSource::User;
            nrd::ResourceType userType = nrd::ResourceType::MAX_NUM;
            std::uint16_t poolIndex = 0;
        };

        struct NrdDispatchResourcePlan {
            nrd::DescriptorType descriptorType = nrd::DescriptorType::TEXTURE;
            std::uint32_t bindingSlot = 0;
            NrdResourceReference resource;
        };

        struct NrdDispatchPlan {
            std::string name;
            std::uint16_t pipelineIndex = 0;
            std::uint16_t gridWidth = 0;
            std::uint16_t gridHeight = 0;
            std::vector<NrdDispatchResourcePlan> resources;
            std::vector<std::uint8_t> constantData;
            bool constantDataMatchesPreviousDispatch = false;
        };

        struct NrdPipelineBindingLayouts {
            nvrhi::BindingLayoutDesc resources;
            nvrhi::BindingLayoutDesc constantsAndSamplers;
        };

        struct NrdDispatchGraphBinding {
            FrameGraphResourceHandle resource;
            nrd::DescriptorType descriptorType = nrd::DescriptorType::TEXTURE;
        };

        /** 将 NRD texture format 转换为 NvRHI format；无等价格式时抛出。 */
        [[nodiscard]] nvrhi::Format nrdFormatToNvrhi(nrd::Format format);

        /** 返回当前 NRD normal encoding 推荐的物理纹理格式。 */
        [[nodiscard]] nvrhi::Format nrdNormalEncodingFormat(nrd::NormalEncoding encoding);

        /** 将 NRD pool 描述转换为实际分辨率的 SRV/UAV 纹理描述。 */
        [[nodiscard]] NrdPoolTexturePlan makeNrdPoolTexturePlan(NrdPoolKind pool, std::uint16_t index,
                                                                const nrd::TextureDesc& texture,
                                                                core::RenderExtent extent, const char* debugName);

        /** 校验 pool index 并把 NRD ResourceDesc 转为稳定资源引用。 */
        [[nodiscard]] NrdResourceReference translateNrdResource(const nrd::ResourceDesc& resource,
                                                                const nrd::InstanceDesc& instanceDesc);

        /** 构造与内嵌 SPIR-V descriptor set/register 完全一致的两个 NvRHI layout。 */
        [[nodiscard]] NrdPipelineBindingLayouts makeNrdPipelineBindingLayouts(const nrd::LibraryDesc& libraryDesc,
                                                                              const nrd::InstanceDesc& instanceDesc,
                                                                              const nrd::PipelineDesc& pipelineDesc);

        /** 深拷贝 NRD 临时 dispatch 数组并完成 resource range/binding slot 翻译。 */
        [[nodiscard]] std::vector<NrdDispatchPlan> makeNrdDispatchPlans(const nrd::InstanceDesc& instanceDesc,
                                                                        std::span<const nrd::DispatchDesc> dispatches);

        /**
         * 为一条 command list 规划 volatile constant-buffer 写入。
         *
         * NvRHI 要求 command list 在首次绑定 volatile buffer 前至少创建一个 version；因此首个 dispatch
         * 没有 NRD 常量时会得到 16 字节零上传。后续标记为复用上一 dispatch 的常量时返回空上传。
         */
        [[nodiscard]] std::vector<std::vector<std::uint8_t>>
        makeNrdConstantUploads(std::span<const NrdDispatchPlan> dispatches);

        /** 由稳定帧输入生成 column-major、screen-UV motion 的 NRD CommonSettings。 */
        [[nodiscard]] nrd::CommonSettings makeNrdCommonSettings(const NrdFrameParameters& parameters,
                                                                const NrdHistoryPlan& historyPlan);

        /**
         * 注册一个严格串行的 NRD compute pass，并为全部 SRV/UAV 声明 FrameGraph 访问。
         * `dependency` 有效时强制保持 NRD 返回的 dispatch 顺序。
         */
        [[nodiscard]] FrameGraphPassHandle
        addNrdDispatchPass(FrameGraph& frameGraph, std::string name, std::span<const NrdDispatchGraphBinding> resources,
                           FrameGraphResourceHandle constantBuffer, bool writesConstantBuffer,
                           FrameGraphPassHandle dependency, FrameGraph::ExecuteCallback execute);

        template <typename CommandList>
        void recordNrdDispatch(CommandList& commandList, const nvrhi::ComputeState& state, std::uint32_t gridWidth,
                               std::uint32_t gridHeight) {
            commandList.setComputeState(state);
            commandList.dispatch(gridWidth, gridHeight, 1);
        }

    } // namespace detail

    /**
     * @brief NRD 4.17.3 REBLUR_DIFFUSE_SPECULAR 的 NvRHI/FrameGraph adapter。
     *
     * 构造阶段直接消费 NRD `InstanceDesc` 与内嵌 SPIR-V，创建 permanent/transient pool、sampler、
     * binding layout 和 compute pipeline，不依赖 NRI。每次 `record()` 只能发生在对应帧槽 fence 已等待后；
     * adapter 会为该槽写入独立 constant buffer 与 binding set，并把每个 NRD dispatch 注册成独立 compute pass。
     */
    class NrdDenoiser final {
    public:
        explicit NrdDenoiser(const NrdDenoiserCreateInfo& createInfo);
        ~NrdDenoiser();

        NrdDenoiser(const NrdDenoiser&) = delete;
        NrdDenoiser& operator=(const NrdDenoiser&) = delete;

        /** 生成并录制本帧全部 REBLUR dispatch，但不推进任何已提交历史。 */
        [[nodiscard]] NrdPreparedFrame record(FrameGraph& frameGraph, const NrdFrameParameters& parameters,
                                              const NrdSignalBindings& signals);

        /** 仅在包含该票据命令的 queue submit 成功后发布输出与双域历史。 */
        void commitSubmittedFrame(const NrdPreparedFrame& frame);

        /**
         * 放弃匹配票据的未提交录制并恢复 NRD pool ping-pong 的 CPU 规划状态。
         * stale/mismatch 票据会被忽略；恢复分配失败会延迟到下一次 `record()` 重试，不会覆盖原始渲染异常。
         */
        void discardFrame(const NrdPreparedFrame& frame) noexcept;

        /** 在通用异常清理路径中无条件放弃当前候选帧；没有候选帧时为空操作。 */
        void discardPendingFrame() noexcept;

        [[nodiscard]] const NrdOutputResources& outputs() const noexcept;
        [[nodiscard]] nvrhi::Format expectedNormalRoughnessFormat() const noexcept;
        [[nodiscard]] const NrdHistoryState& historyState(core::HistoryDomain domain) const;
        [[nodiscard]] bool hasPendingFrame() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

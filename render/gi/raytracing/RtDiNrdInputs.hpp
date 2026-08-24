#pragma once

#include "render/core/FrameIdentity.hpp"
#include "render/gpu/GpuMaterial.hpp"
#include "render/resources/FrameGraph.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

namespace lumin::render {
    class ShaderLibrary;
}

namespace lumin::render::gi {

    /** 与 `shaders/RtDiNrdInputs.slang` 精确同构的逐帧常量。 */
    struct alignas(16) RtDiNrdInputsConstants {
        /// xyz 为 world-space 相机位置；w 固定为 1。
        glm::vec4 cameraPosition{0.0F, 0.0F, 0.0F, 1.0F};
        /// xy 为 current-previous jitter UV 校正，z 为 NRD denoising range，w 保留。
        glm::vec4 renderParameters{0.0F, 0.0F, 500000.0F, 0.0F};
        /// xy 为输出尺寸，z 为材质记录数，w 为无效材质索引。
        glm::uvec4 renderInfo{1U, 1U, 0U, 0xffffffffU};
    };

    static_assert(std::is_standard_layout_v<RtDiNrdInputsConstants>);
    static_assert(sizeof(RtDiNrdInputsConstants) == 48);
    static_assert(alignof(RtDiNrdInputsConstants) == 16);
    static_assert(offsetof(RtDiNrdInputsConstants, cameraPosition) == 0);
    static_assert(offsetof(RtDiNrdInputsConstants, renderParameters) == 16);
    static_assert(offsetof(RtDiNrdInputsConstants, renderInfo) == 32);

    /** RTDI preparation pass 生成的五类 REBLUR 信号格式。 */
    struct RtDiNrdSignalFormats {
        nvrhi::Format diffuseRadianceHitDistance = nvrhi::Format::RGBA16_FLOAT;
        nvrhi::Format specularRadianceHitDistance = nvrhi::Format::RGBA16_FLOAT;
        nvrhi::Format viewZ = nvrhi::Format::R32_FLOAT;
        nvrhi::Format normalRoughness = nvrhi::Format::R10G10B10A2_UNORM;
        nvrhi::Format motion = nvrhi::Format::RG16_FLOAT;
    };

    /** preparation pass 读取的 RTDI raw 波瓣、primary surface 与材质表。 */
    struct RtDiNrdInputResources {
        nvrhi::TextureHandle diffuseRadianceHitT;
        nvrhi::TextureHandle specularRadianceHitT;
        nvrhi::TextureHandle position;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle albedoMetallic;
        nvrhi::TextureHandle materialId;
        nvrhi::TextureHandle viewZ;
        nvrhi::TextureHandle motion;
        nvrhi::BufferHandle materials;
    };

    /** `RtDiNrdInputResources` 在当前 FrameGraph 中的唯一资源身份。 */
    struct RtDiNrdInputGraphResources {
        FrameGraphResourceHandle diffuseRadianceHitT;
        FrameGraphResourceHandle specularRadianceHitT;
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        FrameGraphResourceHandle materialId;
        FrameGraphResourceHandle viewZ;
        FrameGraphResourceHandle motion;
        FrameGraphResourceHandle materials;
        /// RTDI 写完全部输入的 pass；有效时作为显式依赖。
        FrameGraphPassHandle readyPass;
    };

    /** 每个帧槽由 preparation pass 独占的持久化 NRD 输入纹理。 */
    struct RtDiNrdSignalResources {
        nvrhi::TextureHandle diffuseRadianceHitDistance;
        nvrhi::TextureHandle specularRadianceHitDistance;
        nvrhi::TextureHandle viewZ;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle motion;
    };

    /** 本帧 preparation 输出及其完成 pass。 */
    struct RtDiNrdGraphOutput {
        FrameGraphResourceHandle diffuseRadianceHitDistance;
        FrameGraphResourceHandle specularRadianceHitDistance;
        FrameGraphResourceHandle viewZ;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle motion;
        FrameGraphPassHandle readyPass;

        /** 返回五类信号与完成 pass 是否全部有效。 */
        [[nodiscard]] bool isValid() const noexcept;
    };

    /** 单帧 direct-lighting NRD preparation 参数。 */
    struct RtDiNrdFrameParameters {
        core::FrameSlotIndex frameSlot;
        core::RenderExtent extent;
        glm::vec3 cameraPosition{0.0F};
        glm::vec2 jitterDeltaUv{0.0F};
        float denoisingRange = 500000.0F;
        /// 写入帧槽资源前必须已经等待对应 fence。
        bool frameSlotFenceWaited = false;
    };

    /** 创建 direct-lighting NRD preparation pipeline 所需参数。 */
    struct RtDiNrdInputsCreateInfo {
        /** NvRHI 设备；生命周期必须覆盖 pass。 */
        nvrhi::IDevice* device = nullptr;
        /** Session 级 shader 缓存；生命周期必须覆盖创建过程。 */
        ShaderLibrary* shaders = nullptr;
        /** 固定渲染尺寸。 */
        core::RenderExtent extent;
        /** 可同时在途的帧槽数量。 */
        std::uint32_t frameSlotCount = 0;
    };

    /** 向上取整后的 8x8 compute dispatch 数量。 */
    struct RtDiNrdDispatchSize {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t z = 1;

        friend constexpr bool operator==(const RtDiNrdDispatchSize&, const RtDiNrdDispatchSize&) noexcept = default;
    };

    namespace detail {

        /** 构造一张 FrameGraph 管理状态的 SRV/UAV NRD 信号纹理。 */
        [[nodiscard]] nvrhi::TextureDesc makeRtDiNrdSignalTextureDesc(std::uint32_t width, std::uint32_t height,
                                                                      nvrhi::Format format, const char* debugName);

        /** 构造与 `RtDiNrdInputs.slang` set 0 精确同构的 compute layout。 */
        [[nodiscard]] nvrhi::BindingLayoutDesc makeRtDiNrdInputsBindingLayoutDesc();

        /** 校验物理资源并构造 preparation binding set。 */
        [[nodiscard]] nvrhi::BindingSetDesc makeRtDiNrdInputsBindingSetDesc(
            const RtDiNrdInputResources& inputs, const RtDiNrdSignalResources& outputs, nvrhi::IBuffer* constants);

        /** 校验输入尺寸/格式并返回材质记录数。 */
        [[nodiscard]] std::uint32_t validateRtDiNrdInputResources(const RtDiNrdInputResources& inputs,
                                                                  core::RenderExtent extent);

        /** 构造 shader 常量并拒绝非有限参数。 */
        [[nodiscard]] RtDiNrdInputsConstants makeRtDiNrdInputsConstants(const RtDiNrdFrameParameters& parameters,
                                                                        std::uint32_t materialCount);

        /** 根据固定 8x8 线程组计算覆盖整个输出的 dispatch。 */
        [[nodiscard]] RtDiNrdDispatchSize makeRtDiNrdDispatchSize(core::RenderExtent extent);

        template <typename CommandList>
        void recordRtDiNrdDispatch(CommandList& commandList, const nvrhi::ComputeState& state,
                                   RtDiNrdDispatchSize dispatch) {
            commandList.setComputeState(state);
            commandList.dispatch(dispatch.x, dispatch.y, dispatch.z);
        }

    } // namespace detail

    /**
     * @brief 把 RTDI raw diffuse/specular 波瓣转换为 `REBLUR_DIFFUSE_SPECULAR` 输入。
     *
     * pass 独占输出纹理和帧槽常量，但不拥有输入。只有成功 queue submit 后才可调用
     * `commitSubmittedFrame()` 发布资源状态；失败路径必须调用 `discardPendingFrame()`。
     */
    class RtDiNrdInputsPass final {
    public:
        explicit RtDiNrdInputsPass(const RtDiNrdInputsCreateInfo& createInfo);
        ~RtDiNrdInputsPass();

        RtDiNrdInputsPass(const RtDiNrdInputsPass&) = delete;
        RtDiNrdInputsPass& operator=(const RtDiNrdInputsPass&) = delete;

        /** 录制单个 preparation compute pass。 */
        [[nodiscard]] RtDiNrdGraphOutput record(FrameGraph& frameGraph, const RtDiNrdFrameParameters& parameters,
                                                const RtDiNrdInputResources& inputs,
                                                const RtDiNrdInputGraphResources& graphInputs);

        /** queue submit 成功后发布本帧槽输出状态。 */
        void commitSubmittedFrame();
        /** 录制、执行或提交失败时放弃候选状态。 */
        void discardPendingFrame() noexcept;
        /** 返回是否存在尚未提交或放弃的候选帧。 */
        [[nodiscard]] bool hasPendingFrame() const noexcept;

        /** 返回指定帧槽的五类物理输出。 */
        [[nodiscard]] const RtDiNrdSignalResources& resources(std::uint32_t frameSlot) const;
        /** 返回固定 NRD signal formats。 */
        [[nodiscard]] RtDiNrdSignalFormats formats() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

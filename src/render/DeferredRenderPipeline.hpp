#pragma once

#include "render/core/RenderFeaturePipeline.hpp"

#include <cstdint>
#include <functional>

namespace lumin::render {

    /**
     * @brief 大气系统未来用于分别判断 LUT 重建范围的输入签名。
     *
     * 光学签名对应行星与介质参数，灯光签名对应太阳等外部照明，视图签名对应观察高度与方向。
     * 当前阶段只把签名发布到帧黑板，不创建或更新大气 LUT。
     */
    struct AtmosphereInvalidationSignatures {
        /// 行星半径、散射/吸收系数等光学参数签名。
        std::uint64_t optical = 0;

        /// 太阳方向、颜色和强度等灯光输入签名。
        std::uint64_t lighting = 0;

        /// 相机高度、方向及投影视图输入签名。
        std::uint64_t view = 0;

        friend constexpr bool operator==(const AtmosphereInvalidationSignatures&,
                                         const AtmosphereInvalidationSignatures&) noexcept = default;
    };

    /** 单个延迟渲染 Feature 的生命周期回调。 */
    struct DeferredRenderFeatureCallbacks {
        /// 声明该 Feature 的 FrameGraph pass；必须提供。
        std::function<void(core::RenderFeatureFrameContext&)> addPasses;

        /// 对应帧成功提交后的通知；回调不得抛出异常。
        std::function<void(const core::RenderFrameIdentity&)> onSubmitted;

        /// 对应帧被放弃时的通知；回调不得抛出异常。
        std::function<void(const core::RenderFrameIdentity&)> onDiscarded;
    };

    /** 延迟路径或 primary-ray Hybrid 路径的固定 Feature 拓扑。 */
    enum class DeferredRenderPath : std::uint8_t {
        /// 传统 shadow + G-buffer + deferred lighting 路径。
        Raster,
        /// primary RT surface + GI + compute composite 路径，不注册 shadow/G-buffer Feature。
        Hybrid,
    };

    /** 延迟渲染主线 Feature 的回调集合；Hybrid 额外启用 `hybridSurface`。 */
    struct DeferredRenderPipelineCallbacks {
        DeferredRenderFeatureCallbacks shadow;
        DeferredRenderFeatureCallbacks gbuffer;
        /// Hybrid 路径的 primary-ray surface/RTDI Feature；Raster 路径不调用。
        DeferredRenderFeatureCallbacks hybridSurface;
        DeferredRenderFeatureCallbacks atmosphereLuts;
        DeferredRenderFeatureCallbacks globalIllumination;
        DeferredRenderFeatureCallbacks giDenoiser;
        DeferredRenderFeatureCallbacks skyComposite;
        DeferredRenderFeatureCallbacks directLighting;
        DeferredRenderFeatureCallbacks temporalAa;
        DeferredRenderFeatureCallbacks toneMapping;
        DeferredRenderFeatureCallbacks uiPresent;
    };

    /**
     * @brief 延迟渲染主线的固定 Feature 规划门面。
     *
     * 该类型只定义 Feature 身份、依赖和历史域所有权；具体 NvRHI 资源与 pass 仍由宿主回调提供。
     * Raster 执行顺序为 shadow、gbuffer、atmosphere LUT、GI、GI denoiser、sky/composite、direct-lighting、TAA、
     * tonemap、UI/present；Hybrid 会在 GI 前插入 primary RT surface。LUT 必须在 RT miss、SHARC 更新和
     * raster sky 之前就绪。
     */
    class DeferredRenderPipeline final {
    public:
        /**
         * @brief 注册主线 Feature 并按设备能力解析。
         * @throws std::invalid_argument 任一 `addPasses` 回调为空时抛出。
         */
        DeferredRenderPipeline(DeferredRenderPipelineCallbacks callbacks,
                               const core::RenderDeviceCapabilities& capabilities,
                               DeferredRenderPath path = DeferredRenderPath::Raster);

        DeferredRenderPipeline(const DeferredRenderPipeline&) = delete;
        DeferredRenderPipeline& operator=(const DeferredRenderPipeline&) = delete;
        DeferredRenderPipeline(DeferredRenderPipeline&&) = delete;
        DeferredRenderPipeline& operator=(DeferredRenderPipeline&&) = delete;

        /** 根据强类型帧身份与变化集合构建本帧全部 Feature。 */
        void prepareFrame(const core::RenderFrameIdentity& identity, std::uint64_t cameraCutEpoch,
                          const core::FrameChangeSet& changes, FrameGraph& frameGraph,
                          core::RenderBlackboard& blackboard);

        /** GPU 提交成功后原子推进 Feature 与分域历史。 */
        void commitFrame(const core::RenderFrameIdentity& identity);

        /** 放弃录制或提交失败的帧；全部一次性变化会留待重试。 */
        void discardFrame() noexcept;

        /// 返回解析后的 Feature 计划。
        [[nodiscard]] const core::ResolvedRenderFeaturePlan& resolvedPlan() const noexcept;

        /// 返回底层管线是否持有待提交帧。
        [[nodiscard]] bool hasPendingFrame() const noexcept;

        /// 返回指定历史域最近一次成功提交后的状态。
        [[nodiscard]] const core::HistoryDomainState& historyState(core::HistoryDomain domain) const;

        /// 返回当前管线拓扑；可用于诊断和禁止跨路径复用资源。
        [[nodiscard]] DeferredRenderPath path() const noexcept;

    private:
        core::RenderFeaturePipeline pipeline_;
        DeferredRenderPath path_ = DeferredRenderPath::Raster;
    };

} // namespace lumin::render

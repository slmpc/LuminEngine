#pragma once

#include "render/FrameGraph.hpp"
#include "render/core/FrameIdentity.hpp"
#include "render/core/History.hpp"
#include "render/core/RenderBlackboard.hpp"
#include "render/core/RenderFeaturePlan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lumin::render::core {

    /**
     * @brief 向单个渲染 Feature 提供一帧内有效的后端无关上下文。
     *
     * Feature 只能通过 `frameGraph` 声明本帧 pass 和资源状态，通过 `blackboard` 交换强类型数据。
     * 上下文不拥有任何引用；Feature 不得在 `addPasses()` 返回后保存这些引用。
     */
    class RenderFeatureFrameContext final {
    public:
        /// 构造完整帧上下文；通常仅由 `RenderFeaturePipeline` 调用。
        RenderFeatureFrameContext(const RenderFrameIdentity& frameIdentity, const HistoryFramePlan& historyPlan,
                                  FrameGraph& graph, RenderBlackboard& frameBlackboard);

        /// 返回强类型帧身份。
        [[nodiscard]] const RenderFrameIdentity& identity() const noexcept;

        /// 返回本帧相对最近一次成功提交帧的变化集合。
        [[nodiscard]] const FrameChangeSet& changes() const noexcept;

        /**
         * @brief 返回指定历史域在本帧录制前需要执行的动作。
         * @throws std::invalid_argument `domain` 不是有效历史域时抛出。
         */
        [[nodiscard]] HistoryAction historyAction(HistoryDomain domain) const;

        /// 返回本帧唯一的 FrameGraph；运行时 barrier 只能通过它声明。
        [[nodiscard]] FrameGraph& frameGraph() const noexcept;

        /// 返回用于 Feature 间交换强类型帧数据的黑板。
        [[nodiscard]] RenderBlackboard& blackboard() const noexcept;

    private:
        const RenderFrameIdentity& identity_;
        const HistoryFramePlan& historyPlan_;
        FrameGraph& frameGraph_;
        RenderBlackboard& blackboard_;
    };

    /**
     * @brief 可由 `RenderFeaturePipeline` 规划和逐帧调度的渲染功能接口。
     *
     * `addPasses()` 只构建本帧 FrameGraph，不得推进跨帧历史。历史及其他提交状态只能在
     * `onFrameSubmitted()` 中推进；录制、提交或 present 前失败时会调用 `onFrameDiscarded()`。
     */
    class IRenderFeature {
    public:
        IRenderFeature() = default;
        virtual ~IRenderFeature() = default;

        IRenderFeature(const IRenderFeature&) = delete;
        IRenderFeature& operator=(const IRenderFeature&) = delete;
        IRenderFeature(IRenderFeature&&) = delete;
        IRenderFeature& operator=(IRenderFeature&&) = delete;

        /// 返回 Feature 的静态规划描述符；引用必须在 Feature 生命周期内保持有效。
        [[nodiscard]] virtual const FeatureDescriptor& descriptor() const noexcept = 0;

        /// 向本帧 FrameGraph 注册 pass，并按需读取或发布 blackboard 数据。
        virtual void addPasses(RenderFeatureFrameContext& context) = 0;

        /// 在本帧成功提交后推进跨帧状态；实现不得抛出异常。
        virtual void onFrameSubmitted(const RenderFrameIdentity& identity) noexcept;

        /// 在已开始构建的帧未提交时回滚暂存状态；实现不得抛出异常。
        virtual void onFrameDiscarded(const RenderFrameIdentity& identity) noexcept;
    };

    /**
     * @brief 将 Feature 依赖解析、逐帧 pass 构建和提交事务组合为统一调度器。
     *
     * 管线拥有注册的 Feature。`resolve()` 根据设备能力生成固定执行顺序；每次 `prepareFrame()`
     * 必须由一次 `commitFrame()` 或 `discardFrame()` 结束。只有 `commitFrame()` 会通知 Feature
     * 推进跨帧历史，从而保证失败帧不会污染 TAA、NRD、SHARC 或大气 LUT 状态。
     */
    class RenderFeaturePipeline final {
    public:
        /// 使用引擎标准分域历史策略构造空管线。
        RenderFeaturePipeline();

        /// 使用调用方提供的历史策略构造空管线。
        explicit RenderFeaturePipeline(HistoryPolicyMap historyPolicies);

        /// 销毁管线；存在未提交帧时会先执行 discard 通知。
        ~RenderFeaturePipeline();

        RenderFeaturePipeline(const RenderFeaturePipeline&) = delete;
        RenderFeaturePipeline& operator=(const RenderFeaturePipeline&) = delete;
        RenderFeaturePipeline(RenderFeaturePipeline&&) = delete;
        RenderFeaturePipeline& operator=(RenderFeaturePipeline&&) = delete;

        /**
         * @brief 转移并注册一个 Feature。
         * @throws std::invalid_argument 指针为空或标识重复时抛出。
         *
         * 注册新 Feature 会使已有解析结果失效，调用方必须重新执行 `resolve()`。
         */
        void addFeature(std::unique_ptr<IRenderFeature> feature);

        /// 移除全部 Feature 和解析结果；存在待提交帧时先执行 discard。
        void clear() noexcept;

        /**
         * @brief 根据设备能力解析依赖和降级策略，并建立固定执行顺序。
         * @throws std::logic_error 当前仍有待提交帧时抛出。
         * @throws std::invalid_argument Feature 契约无效时抛出。
         * @throws std::runtime_error 严格 Feature 的要求无法满足时抛出。
         */
        void resolve(const RenderDeviceCapabilities& capabilities);

        /// 返回最近一次成功解析的不可变计划；尚未解析时返回 `nullptr`。
        [[nodiscard]] const ResolvedRenderFeaturePlan* resolvedPlan() const noexcept;

        /// 返回当前解析结果中的启用 Feature，顺序即逐帧执行顺序。
        [[nodiscard]] std::span<IRenderFeature* const> activeFeatures() const noexcept;

        /**
         * @brief 为一帧计算历史动作并按解析顺序调用全部启用 Feature。
         * @throws std::logic_error 管线未解析或已有待提交帧时抛出。
         * @throws std::invalid_argument 帧身份无效时抛出。
         *
         * 任一 Feature 抛出异常时，已进入的 Feature 会按逆序收到 discard 通知，异常继续向上传播。
         */
        void prepareFrame(const RenderFrameIdentity& identity, std::uint64_t cameraCutEpoch,
                          const FrameChangeSet& changes, FrameGraph& frameGraph, RenderBlackboard& blackboard);

        /**
         * @brief 确认待提交帧已经成功提交，并按执行顺序通知全部 Feature。
         * @throws std::logic_error 不存在待提交帧或身份不匹配时抛出。
         */
        void commitFrame(const RenderFrameIdentity& identity);

        /// 放弃当前待提交帧；无待提交帧时不执行操作。
        void discardFrame() noexcept;

        /// 返回当前是否存在已经构建但尚未确认提交的帧。
        [[nodiscard]] bool hasPendingFrame() const noexcept;

        /**
         * @brief 返回指定历史域最近一次成功提交后的状态。
         * @throws std::invalid_argument `domain` 不是有效历史域时抛出。
         */
        [[nodiscard]] const HistoryDomainState& historyState(HistoryDomain domain) const;

    private:
        void discardPreparedFeatures() noexcept;
        void invalidateResolution() noexcept;

        HistoryCoordinator historyCoordinator_;
        std::vector<std::unique_ptr<IRenderFeature>> features_;
        std::optional<ResolvedRenderFeaturePlan> resolvedPlan_;
        std::vector<IRenderFeature*> activeFeatures_;
        std::optional<RenderFrameIdentity> pendingIdentity_;
        std::size_t preparedFeatureCount_ = 0;
    };

} // namespace lumin::render::core

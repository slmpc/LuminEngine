#pragma once

#include "render/core/RenderFeatureRegistry.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lumin::render::core {

    /**
     * @brief 拥有一条已解析 recipe 的全部 Feature 实例和跨帧事务状态。
     *
     * 构造阶段按 DAG 顺序创建并初始化 Feature；任一步失败都会逆序调用已进入实例的 `shutdown()`。
     * 每次 `prepareFrame()` 必须由 `commitFrame()` 或 `discardFrame()` 结束，只有成功提交才推进历史。
     * 该对象不提供内部同步，全部方法必须由渲染主线程调用。
     */
    class RenderPipelineInstance final {
    public:
        /**
         * @brief 使用标准历史策略创建候选 PipelineInstance。
         * @throws std::invalid_argument 创建上下文或解析结果无效时抛出。
         * @throws std::exception Factory 或 Feature 初始化失败时在完整回滚后继续抛出。
         */
        RenderPipelineInstance(const RenderFeatureRegistry& registry, const ResolvedRenderPipeline& resolved,
                               const FeatureCreateContext& createContext);

        /**
         * @brief 使用指定历史策略创建候选 PipelineInstance。
         * @throws std::invalid_argument 创建上下文或解析结果无效时抛出。
         * @throws std::exception Factory 或 Feature 初始化失败时在完整回滚后继续抛出。
         */
        RenderPipelineInstance(const RenderFeatureRegistry& registry, const ResolvedRenderPipeline& resolved,
                               const FeatureCreateContext& createContext, HistoryPolicyMap historyPolicies);

        /// 放弃待提交帧并逆序关闭全部 Feature。
        ~RenderPipelineInstance();

        RenderPipelineInstance(const RenderPipelineInstance&) = delete;
        RenderPipelineInstance& operator=(const RenderPipelineInstance&) = delete;
        RenderPipelineInstance(RenderPipelineInstance&&) = delete;
        RenderPipelineInstance& operator=(RenderPipelineInstance&&) = delete;

        /// 返回当前实例对应的 recipe 标识。
        [[nodiscard]] const std::string& recipeId() const noexcept;

        /// 返回按 DAG 顺序排列的 Feature；指针只在实例关闭前有效。
        [[nodiscard]] std::span<IRenderFeature* const> features() const noexcept;

        /**
         * @brief 为一帧计算历史动作并按 DAG 顺序构建 pass。
         * @throws std::logic_error 实例已关闭、不可继续使用或已有待提交帧时抛出。
         * @throws std::invalid_argument 帧身份无效时抛出。
         *
         * Feature 抛出时，所有已进入 Feature 会逆序收到 `onFrameDiscarded()`，异常继续传播。
         */
        void prepareFrame(const RenderFrameIdentity& identity, std::uint64_t cameraCutEpoch,
                          const FrameChangeSet& changes, FrameGraph& frameGraph, RenderBlackboard& blackboard);

        /**
         * @brief 确认 GPU 已接受待提交帧并推进 Feature 与历史状态。
         * @throws std::logic_error 没有待提交帧或身份不匹配时抛出。
         */
        void commitFrame(const RenderFrameIdentity& identity);

        /// 放弃待提交帧；没有待提交帧时不执行操作。
        void discardFrame() noexcept;

        /// 返回当前是否存在已构建但尚未确认提交的帧。
        [[nodiscard]] bool hasPendingFrame() const noexcept;

        /**
         * @brief 在安全帧边界逆序通知 Feature 更新尺寸相关资源。
         * @throws std::logic_error 存在待提交帧、实例已关闭或此前尺寸更新失败时抛出。
         * @throws std::invalid_argument `extent` 为空时抛出。
         *
         * 任一 Feature 抛出后实例会标记为不可继续渲染，Runtime 应关闭该候选并保留旧实例。
         */
        void onRenderExtentChanged(RenderExtent extent);

        /// 返回最近一次成功应用的渲染范围；尚未通知时返回空值。
        [[nodiscard]] std::optional<RenderExtent> renderExtent() const noexcept;

        /// 返回实例是否仍可接受新帧；尺寸更新失败后返回 `false`。
        [[nodiscard]] bool isUsable() const noexcept;

        /**
         * @brief 放弃待提交帧并逆序释放所有 Feature 资源。
         *
         * 可重复调用；首次调用后实例不再接受任何渲染或尺寸事件。
         */
        void shutdown() noexcept;

        /**
         * @brief 返回指定历史域最近一次成功提交后的状态。
         * @throws std::invalid_argument `domain` 不是有效历史域时抛出。
         */
        [[nodiscard]] const HistoryDomainState& historyState(HistoryDomain domain) const;

    private:
        void discardPreparedFeatures() noexcept;
        void shutdownFeatures() noexcept;

        std::string recipeId_;
        HistoryCoordinator historyCoordinator_;
        std::vector<std::unique_ptr<IRenderFeature>> ownedFeatures_;
        std::vector<IRenderFeature*> featureViews_;
        std::optional<RenderFrameIdentity> pendingIdentity_;
        std::optional<RenderExtent> renderExtent_;
        std::size_t preparedFeatureCount_ = 0;
        bool usable_ = true;
        bool shutdown_ = false;
    };

} // namespace lumin::render::core

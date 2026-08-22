#include "render/core/RenderPipelineInstance.hpp"

#include <stdexcept>
#include <utility>

namespace lumin::render::core {

    RenderPipelineInstance::RenderPipelineInstance(const RenderFeatureRegistry& registry,
                                                   const ResolvedRenderPipeline& resolved,
                                                   const FeatureCreateContext& createContext)
        : RenderPipelineInstance(registry, resolved, createContext, makeDefaultHistoryPolicyMap()) {
    }

    RenderPipelineInstance::RenderPipelineInstance(const RenderFeatureRegistry& registry,
                                                   const ResolvedRenderPipeline& resolved,
                                                   const FeatureCreateContext& createContext,
                                                   HistoryPolicyMap historyPolicies)
        : recipeId_(resolved.id()), historyCoordinator_(std::move(historyPolicies)) {
        if (recipeId_.empty()) {
            throw std::invalid_argument("Render PipelineInstance requires a resolved recipe id.");
        }
        if (createContext.frameSlotCount == 0) {
            throw std::invalid_argument("Render PipelineInstance requires at least one frame slot.");
        }

        ownedFeatures_.reserve(resolved.executionOrder().size());
        featureViews_.reserve(resolved.executionOrder().size());
        try {
            for (const FeatureId& id : resolved.executionOrder()) {
                std::unique_ptr<IRenderFeature> feature = registry.create(id, createContext);
                ownedFeatures_.push_back(std::move(feature));
                ownedFeatures_.back()->initialize(createContext);
                featureViews_.push_back(ownedFeatures_.back().get());
            }
        } catch (...) {
            // 初始化失败时必须先释放下游候选资源，旧实例才能保持完全不受影响。
            shutdownFeatures();
            throw;
        }
    }

    RenderPipelineInstance::~RenderPipelineInstance() {
        shutdown();
    }

    const std::string& RenderPipelineInstance::recipeId() const noexcept {
        return recipeId_;
    }

    std::span<IRenderFeature* const> RenderPipelineInstance::features() const noexcept {
        return featureViews_;
    }

    void RenderPipelineInstance::prepareFrame(const RenderFrameIdentity& identity, std::uint64_t cameraCutEpoch,
                                              const FrameChangeSet& changes, FrameGraph& frameGraph,
                                              RenderBlackboard& blackboard) {
        if (shutdown_) {
            throw std::logic_error("Render PipelineInstance is shut down.");
        }
        if (!usable_) {
            throw std::logic_error("Render PipelineInstance is unusable after a failed extent update.");
        }
        if (hasPendingFrame()) {
            throw std::logic_error("Render PipelineInstance already has a pending frame.");
        }
        if (!identity.isValid()) {
            throw std::invalid_argument("Render PipelineInstance requires a valid frame identity.");
        }

        const HistoryFramePlan historyPlan = historyCoordinator_.beginFrame(
            HistoryFrameObservation{identity.sequence, cameraCutEpoch, identity.extent, changes});
        RenderFeatureFrameContext context{identity, historyPlan, frameGraph, blackboard};
        pendingIdentity_ = identity;
        preparedFeatureCount_ = 0;
        try {
            for (std::size_t index = 0; index < featureViews_.size(); ++index) {
                preparedFeatureCount_ = index + 1;
                featureViews_[index]->addPasses(context);
            }
        } catch (...) {
            discardPreparedFeatures();
            throw;
        }
    }

    void RenderPipelineInstance::commitFrame(const RenderFrameIdentity& identity) {
        if (!pendingIdentity_) {
            throw std::logic_error("Render PipelineInstance has no pending frame to commit.");
        }
        if (*pendingIdentity_ != identity) {
            throw std::logic_error("Render PipelineInstance commit identity does not match the pending frame.");
        }
        for (std::size_t index = 0; index < preparedFeatureCount_; ++index) {
            featureViews_[index]->onFrameSubmitted(identity);
        }
        historyCoordinator_.commitSubmittedFrame(identity.sequence);
        preparedFeatureCount_ = 0;
        pendingIdentity_.reset();
    }

    void RenderPipelineInstance::discardFrame() noexcept {
        if (pendingIdentity_) {
            discardPreparedFeatures();
        }
    }

    bool RenderPipelineInstance::hasPendingFrame() const noexcept {
        return pendingIdentity_.has_value();
    }

    void RenderPipelineInstance::onRenderExtentChanged(RenderExtent extent) {
        if (shutdown_) {
            throw std::logic_error("Render PipelineInstance is shut down.");
        }
        if (!usable_) {
            throw std::logic_error("Render PipelineInstance is unusable after a failed extent update.");
        }
        if (hasPendingFrame()) {
            throw std::logic_error("Render extent cannot change while a frame is pending.");
        }
        if (extent.isEmpty()) {
            throw std::invalid_argument("Render PipelineInstance requires a non-empty render extent.");
        }
        if (renderExtent_ == extent) {
            return;
        }

        try {
            // 消费者先释放尺寸相关引用，随后生产者才能重建其资源。
            for (auto feature = featureViews_.rbegin(); feature != featureViews_.rend(); ++feature) {
                (*feature)->onRenderExtentChanged(extent);
            }
            renderExtent_ = extent;
        } catch (...) {
            usable_ = false;
            throw;
        }
    }

    std::optional<RenderExtent> RenderPipelineInstance::renderExtent() const noexcept {
        return renderExtent_;
    }

    bool RenderPipelineInstance::isUsable() const noexcept {
        return usable_ && !shutdown_;
    }

    void RenderPipelineInstance::shutdown() noexcept {
        if (shutdown_) {
            return;
        }
        discardFrame();
        shutdownFeatures();
        shutdown_ = true;
        usable_ = false;
    }

    const HistoryDomainState& RenderPipelineInstance::historyState(HistoryDomain domain) const {
        return historyCoordinator_.state(domain);
    }

    void RenderPipelineInstance::discardPreparedFeatures() noexcept {
        const RenderFrameIdentity identity = *pendingIdentity_;
        while (preparedFeatureCount_ > 0) {
            --preparedFeatureCount_;
            featureViews_[preparedFeatureCount_]->onFrameDiscarded(identity);
        }
        historyCoordinator_.abandonFrame(identity.sequence);
        pendingIdentity_.reset();
    }

    void RenderPipelineInstance::shutdownFeatures() noexcept {
        featureViews_.clear();
        while (!ownedFeatures_.empty()) {
            ownedFeatures_.back()->shutdown();
            ownedFeatures_.pop_back();
        }
    }

} // namespace lumin::render::core

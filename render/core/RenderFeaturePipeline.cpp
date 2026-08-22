#include "render/core/RenderFeaturePipeline.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace lumin::render::core {

    void IRenderFeature::initialize(const FeatureCreateContext&) {
    }

    void IRenderFeature::onRenderExtentChanged(RenderExtent) {
    }

    RenderFeatureFrameContext::RenderFeatureFrameContext(const RenderFrameIdentity& frameIdentity,
                                                         const HistoryFramePlan& historyPlan, FrameGraph& graph,
                                                         RenderBlackboard& frameBlackboard)
        : identity_(frameIdentity), historyPlan_(historyPlan), frameGraph_(graph), blackboard_(frameBlackboard) {
        if (!historyPlan_.isValid() || historyPlan_.sequence() != identity_.sequence) {
            throw std::invalid_argument("Render feature context requires a matching history frame plan.");
        }
    }

    const RenderFrameIdentity& RenderFeatureFrameContext::identity() const noexcept {
        return identity_;
    }

    const FrameChangeSet& RenderFeatureFrameContext::changes() const noexcept {
        return historyPlan_.changes();
    }

    HistoryAction RenderFeatureFrameContext::historyAction(HistoryDomain domain) const {
        return historyPlan_.actionFor(domain);
    }

    FrameGraph& RenderFeatureFrameContext::frameGraph() const noexcept {
        return frameGraph_;
    }

    RenderBlackboard& RenderFeatureFrameContext::blackboard() const noexcept {
        return blackboard_;
    }

    void IRenderFeature::onFrameSubmitted(const RenderFrameIdentity&) noexcept {
    }

    void IRenderFeature::onFrameDiscarded(const RenderFrameIdentity&) noexcept {
    }

    void IRenderFeature::shutdown() noexcept {
    }

    RenderFeaturePipeline::RenderFeaturePipeline() : RenderFeaturePipeline(makeDefaultHistoryPolicyMap()) {
    }

    RenderFeaturePipeline::RenderFeaturePipeline(HistoryPolicyMap historyPolicies)
        : historyCoordinator_(std::move(historyPolicies)) {
    }

    RenderFeaturePipeline::~RenderFeaturePipeline() {
        discardFrame();
    }

    void RenderFeaturePipeline::addFeature(std::unique_ptr<IRenderFeature> feature) {
        if (feature == nullptr) {
            throw std::invalid_argument("Render feature pipeline cannot register a null feature.");
        }
        const FeatureId& id = feature->descriptor().id;
        const auto duplicate = std::find_if(features_.begin(), features_.end(), [&id](const auto& existing) {
            return existing->descriptor().id == id;
        });
        if (duplicate != features_.end()) {
            throw std::invalid_argument("Render feature pipeline contains a duplicate feature id: " + id.value());
        }
        if (hasPendingFrame()) {
            throw std::logic_error("Render feature pipeline cannot register a feature while a frame is pending.");
        }
        features_.push_back(std::move(feature));
        invalidateResolution();
    }

    void RenderFeaturePipeline::clear() noexcept {
        discardFrame();
        invalidateResolution();
        features_.clear();
    }

    void RenderFeaturePipeline::resolve(const RenderDeviceCapabilities& capabilities) {
        if (hasPendingFrame()) {
            throw std::logic_error("Render feature pipeline cannot resolve while a frame is pending.");
        }

        RenderFeaturePlan plan;
        std::unordered_map<FeatureId, IRenderFeature*, FeatureIdHash> registry;
        registry.reserve(features_.size());
        for (const auto& feature : features_) {
            plan.addFeature(feature->descriptor());
            registry.emplace(feature->descriptor().id, feature.get());
        }

        ResolvedRenderFeaturePlan resolved = plan.resolve(capabilities);
        std::vector<IRenderFeature*> active;
        active.reserve(resolved.executionOrder().size());
        for (const FeatureId& id : resolved.executionOrder()) {
            const auto iterator = registry.find(id);
            if (iterator == registry.end()) {
                throw std::logic_error("Resolved render feature is absent from the runtime registry.");
            }
            active.push_back(iterator->second);
        }

        resolvedPlan_ = std::move(resolved);
        activeFeatures_ = std::move(active);
    }

    const ResolvedRenderFeaturePlan* RenderFeaturePipeline::resolvedPlan() const noexcept {
        return resolvedPlan_ ? &*resolvedPlan_ : nullptr;
    }

    std::span<IRenderFeature* const> RenderFeaturePipeline::activeFeatures() const noexcept {
        return activeFeatures_;
    }

    void RenderFeaturePipeline::prepareFrame(const RenderFrameIdentity& identity, std::uint64_t cameraCutEpoch,
                                             const FrameChangeSet& changes, FrameGraph& frameGraph,
                                             RenderBlackboard& blackboard) {
        if (!resolvedPlan_) {
            throw std::logic_error("Render feature pipeline must be resolved before preparing a frame.");
        }
        if (hasPendingFrame()) {
            throw std::logic_error("Render feature pipeline already has a pending frame.");
        }
        if (!identity.isValid()) {
            throw std::invalid_argument("Render feature pipeline requires a valid frame identity.");
        }

        const HistoryFramePlan historyPlan = historyCoordinator_.beginFrame(
            HistoryFrameObservation{identity.sequence, cameraCutEpoch, identity.extent, changes});
        RenderFeatureFrameContext context{identity, historyPlan, frameGraph, blackboard};
        pendingIdentity_ = identity;
        preparedFeatureCount_ = 0;
        try {
            for (std::size_t index = 0; index < activeFeatures_.size(); ++index) {
                preparedFeatureCount_ = index + 1;
                activeFeatures_[index]->addPasses(context);
            }
        } catch (...) {
            discardPreparedFeatures();
            throw;
        }
    }

    void RenderFeaturePipeline::commitFrame(const RenderFrameIdentity& identity) {
        if (!pendingIdentity_) {
            throw std::logic_error("Render feature pipeline has no pending frame to commit.");
        }
        if (*pendingIdentity_ != identity) {
            throw std::logic_error("Render feature pipeline commit identity does not match the pending frame.");
        }
        for (std::size_t index = 0; index < preparedFeatureCount_; ++index) {
            activeFeatures_[index]->onFrameSubmitted(identity);
        }
        historyCoordinator_.commitSubmittedFrame(identity.sequence);
        preparedFeatureCount_ = 0;
        pendingIdentity_.reset();
    }

    void RenderFeaturePipeline::discardFrame() noexcept {
        if (!pendingIdentity_) {
            return;
        }
        discardPreparedFeatures();
    }

    bool RenderFeaturePipeline::hasPendingFrame() const noexcept {
        return pendingIdentity_.has_value();
    }

    const HistoryDomainState& RenderFeaturePipeline::historyState(HistoryDomain domain) const {
        return historyCoordinator_.state(domain);
    }

    void RenderFeaturePipeline::discardPreparedFeatures() noexcept {
        const RenderFrameIdentity identity = *pendingIdentity_;
        while (preparedFeatureCount_ > 0) {
            --preparedFeatureCount_;
            activeFeatures_[preparedFeatureCount_]->onFrameDiscarded(identity);
        }
        historyCoordinator_.abandonFrame(identity.sequence);
        pendingIdentity_.reset();
    }

    void RenderFeaturePipeline::invalidateResolution() noexcept {
        resolvedPlan_.reset();
        activeFeatures_.clear();
    }

} // namespace lumin::render::core

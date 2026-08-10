#include "render/core/RenderFeaturePipeline.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using namespace lumin::render;
    using namespace lumin::render::core;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function>
    void requireThrows(Function&& function, const std::string& message) {
        bool rejected = false;
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            rejected = true;
        }
        require(rejected, message);
    }

    struct FeatureProbe {
        std::vector<std::string> events;
        HistoryAction observedHistoryAction = HistoryAction::Keep;
    };

    class ProbeFeature final : public IRenderFeature {
    public:
        ProbeFeature(FeatureDescriptor descriptor, FeatureProbe& probe, bool publishValue = false,
                     bool consumeValue = false, bool throwDuringPrepare = false)
            : descriptor_(std::move(descriptor)), probe_(probe), publishValue_(publishValue),
              consumeValue_(consumeValue), throwDuringPrepare_(throwDuringPrepare) {
        }

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        void addPasses(RenderFeatureFrameContext& context) override {
            probe_.events.push_back("prepare:" + descriptor_.id.value());
            probe_.observedHistoryAction = context.historyAction(HistoryDomain::Taa);
            require(context.identity().sequence.value() == 7, "Feature must receive the prepared frame identity.");
            if (publishValue_) {
                context.blackboard().set(std::uint32_t{41});
            }
            if (consumeValue_) {
                require(context.blackboard().get<std::uint32_t>() == 41,
                        "Dependent features must observe blackboard values from earlier features.");
            }
            if (throwDuringPrepare_) {
                throw std::runtime_error("probe prepare failure");
            }
        }

        void onFrameSubmitted(const RenderFrameIdentity&) noexcept override {
            probe_.events.push_back("submit:" + descriptor_.id.value());
        }

        void onFrameDiscarded(const RenderFrameIdentity&) noexcept override {
            probe_.events.push_back("discard:" + descriptor_.id.value());
        }

    private:
        FeatureDescriptor descriptor_;
        FeatureProbe& probe_;
        bool publishValue_ = false;
        bool consumeValue_ = false;
        bool throwDuringPrepare_ = false;
    };

    [[nodiscard]] RenderFrameIdentity testIdentity(std::uint64_t sequence = 7) {
        return {FrameSlotIndex{0}, SwapImageIndex{1}, RenderSequence{sequence}, {1280, 720}};
    }

    void testResolvedFeaturesBuildAndCommitInDependencyOrder() {
        FeatureProbe probe;
        RenderFeaturePipeline pipeline;

        FeatureDescriptor composite{FeatureId{"composite"}};
        composite.dependencies = {FeatureId{"gbuffer"}};
        pipeline.addFeature(std::make_unique<ProbeFeature>(std::move(composite), probe, false, true));

        FeatureDescriptor rayTracing{FeatureId{"ray-tracing"}};
        rayTracing.requiredCapabilities = {RenderCapability::RayTracingPipeline};
        pipeline.addFeature(std::make_unique<ProbeFeature>(std::move(rayTracing), probe));

        FeatureDescriptor gbuffer{FeatureId{"gbuffer"}};
        gbuffer.requiredCapabilities = {RenderCapability::Graphics};
        gbuffer.historyDomains = {HistoryDomain::Taa};
        pipeline.addFeature(std::make_unique<ProbeFeature>(std::move(gbuffer), probe, true));

        pipeline.resolve(RenderDeviceCapabilities{.supported = {RenderCapability::Graphics}});
        require(pipeline.activeFeatures().size() == 2, "Unavailable RT features must be removed from runtime order.");
        require(pipeline.resolvedPlan()->find(FeatureId{"ray-tracing"})->activation ==
                    FeatureActivation::DisabledMissingCapabilities,
                "Resolved runtime plans must preserve disabled-feature diagnostics.");

        FrameGraph frameGraph;
        RenderBlackboard blackboard;
        pipeline.prepareFrame(testIdentity(), 3, FrameChangeSet{HistoryReason::CameraCut}, frameGraph, blackboard);
        require(pipeline.hasPendingFrame(), "Preparing a frame must open a submission transaction.");
        require(probe.events == std::vector<std::string>{"prepare:gbuffer", "prepare:composite"},
                "Features must prepare in resolved dependency order.");
        require(probe.observedHistoryAction == HistoryAction::FullReset,
                "Features must receive domain-specific history actions.");

        pipeline.commitFrame(testIdentity());
        require(!pipeline.hasPendingFrame(), "Committing a frame must close its transaction.");
        require(probe.events == std::vector<std::string>{"prepare:gbuffer", "prepare:composite", "submit:gbuffer",
                                                         "submit:composite"},
                "Only a successful commit may send submission notifications.");
        require(pipeline.historyState(HistoryDomain::Taa).acceptedFrameCount == 1,
                "Successful commits must advance history exactly once.");
    }

    void testPreparationFailureDiscardsEnteredFeaturesInReverseOrder() {
        FeatureProbe probe;
        RenderFeaturePipeline pipeline;

        FeatureDescriptor first{FeatureId{"first"}};
        pipeline.addFeature(std::make_unique<ProbeFeature>(std::move(first), probe));
        FeatureDescriptor failing{FeatureId{"failing"}};
        failing.dependencies = {FeatureId{"first"}};
        pipeline.addFeature(std::make_unique<ProbeFeature>(std::move(failing), probe, false, false, true));
        pipeline.resolve(RenderDeviceCapabilities{});

        FrameGraph frameGraph;
        RenderBlackboard blackboard;
        requireThrows<std::runtime_error>(
            [&] {
                pipeline.prepareFrame(testIdentity(), 0, FrameChangeSet{}, frameGraph, blackboard);
            },
            "Feature preparation exceptions must propagate to the frame coordinator.");
        require(!pipeline.hasPendingFrame(), "A failed preparation must close the submission transaction.");
        require(probe.events == std::vector<std::string>{"prepare:first", "prepare:failing", "discard:failing",
                                                         "discard:first"},
                "Entered features must receive reverse-order discard notifications.");
    }

    void testExplicitDiscardAndStateValidation() {
        FeatureProbe probe;
        RenderFeaturePipeline pipeline;
        FeatureDescriptor feature{FeatureId{"feature"}};
        pipeline.addFeature(std::make_unique<ProbeFeature>(std::move(feature), probe));

        FrameGraph frameGraph;
        RenderBlackboard blackboard;
        requireThrows<std::logic_error>(
            [&] {
                pipeline.prepareFrame(testIdentity(), 0, FrameChangeSet{}, frameGraph, blackboard);
            },
            "Unresolved pipelines must reject frame preparation.");

        pipeline.resolve(RenderDeviceCapabilities{});
        requireThrows<std::invalid_argument>(
            [&] {
                pipeline.prepareFrame(RenderFrameIdentity{}, 0, FrameChangeSet{}, frameGraph, blackboard);
            },
            "Invalid frame identities must be rejected before Feature callbacks.");

        pipeline.prepareFrame(testIdentity(), 0, FrameChangeSet{}, frameGraph, blackboard);
        requireThrows<std::logic_error>(
            [&] {
                pipeline.commitFrame(testIdentity(8));
            },
            "A different frame identity must not commit the pending transaction.");
        pipeline.discardFrame();
        require(probe.events == std::vector<std::string>{"prepare:feature", "discard:feature"},
                "Explicit discard must not emit a submission notification.");
        require(pipeline.historyState(HistoryDomain::Taa).acceptedFrameCount == 0,
                "Discarded frames must not advance history state.");
        pipeline.discardFrame();

        requireThrows<std::invalid_argument>(
            [&] {
                pipeline.addFeature(nullptr);
            },
            "Null Feature registration must be rejected.");
        FeatureDescriptor duplicate{FeatureId{"feature"}};
        requireThrows<std::invalid_argument>(
            [&] {
                pipeline.addFeature(std::make_unique<ProbeFeature>(std::move(duplicate), probe));
            },
            "Duplicate Feature ids must be rejected during registration.");
    }

} // namespace

int main() {
    try {
        testResolvedFeaturesBuildAndCommitInDependencyOrder();
        testPreparationFailureDiscardsEnteredFeaturesInReverseOrder();
        testExplicitDiscardAndStateValidation();
        std::cout << "Render feature pipeline tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Render feature pipeline test failed: " << exception.what() << '\n';
        return 1;
    }
}

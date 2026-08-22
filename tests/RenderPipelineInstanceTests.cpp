#include "render/core/RenderPipelineInstance.hpp"

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

    struct ProducerOutput {};

    struct LifecycleProbe {
        std::vector<std::string> events;
    };

    class LifecycleFeature final : public IRenderFeature {
    public:
        LifecycleFeature(FeatureDescriptor descriptor, LifecycleProbe& probe, bool failInitialization,
                         bool publishValue, bool consumeValue)
            : descriptor_(std::move(descriptor)), probe_(probe), failInitialization_(failInitialization),
              publishValue_(publishValue), consumeValue_(consumeValue) {
        }

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        void initialize(const FeatureCreateContext& context) override {
            probe_.events.push_back("initialize:" + descriptor_.id.value());
            if (context.frameSlotCount != 2) {
                throw std::runtime_error("Feature did not receive the explicit create context.");
            }
            if (failInitialization_) {
                throw std::runtime_error("expected initialization failure");
            }
        }

        void onRenderExtentChanged(RenderExtent) override {
            probe_.events.push_back("extent:" + descriptor_.id.value());
        }

        void addPasses(RenderFeatureFrameContext& context) override {
            probe_.events.push_back("prepare:" + descriptor_.id.value());
            if (publishValue_) {
                context.blackboard().set(std::uint32_t{73});
            }
            if (consumeValue_ && context.blackboard().get<std::uint32_t>() != 73) {
                throw std::runtime_error("Consumer did not observe the producer value.");
            }
        }

        void onFrameSubmitted(const RenderFrameIdentity&) noexcept override {
            probe_.events.push_back("submitted:" + descriptor_.id.value());
        }

        void onFrameDiscarded(const RenderFrameIdentity&) noexcept override {
            probe_.events.push_back("discarded:" + descriptor_.id.value());
        }

        void shutdown() noexcept override {
            probe_.events.push_back("shutdown:" + descriptor_.id.value());
        }

    private:
        FeatureDescriptor descriptor_;
        LifecycleProbe& probe_;
        bool failInitialization_ = false;
        bool publishValue_ = false;
        bool consumeValue_ = false;
    };

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function> void requireThrows(Function&& function, const char* message) {
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    void registerLifecycleFeature(RenderFeatureRegistry& registry, FeatureDescriptor descriptor, LifecycleProbe& probe,
                                  bool failInitialization = false, bool publishValue = false,
                                  bool consumeValue = false) {
        FeatureDescriptor factoryDescriptor = descriptor;
        const std::string id = descriptor.id.value();
        registry.registerFeature(std::move(descriptor),
                                 [descriptor = std::move(factoryDescriptor), &probe, id, failInitialization,
                                  publishValue, consumeValue](const FeatureCreateContext&) mutable {
                                     probe.events.push_back("factory:" + id);
                                     return std::make_unique<LifecycleFeature>(descriptor, probe, failInitialization,
                                                                               publishValue, consumeValue);
                                 });
    }

    [[nodiscard]] RenderFrameIdentity frameIdentity(std::uint64_t sequence) {
        return {FrameSlotIndex{0}, SwapImageIndex{0}, RenderSequence{sequence}, RenderExtent{1280, 720}};
    }

    void testInitializationFailureRollsBackInReverseOrder() {
        LifecycleProbe probe;
        RenderFeatureRegistry registry;

        FeatureDescriptor producer{FeatureId{"producer"}};
        registerLifecycleFeature(registry, std::move(producer), probe);
        FeatureDescriptor failing{FeatureId{"failing"}};
        failing.dependencies = {FeatureId{"producer"}};
        registerLifecycleFeature(registry, std::move(failing), probe, true);

        const RenderPipelineRecipe recipe{
            .id = "rollback",
            .features = {FeatureId{"producer"}, FeatureId{"failing"}},
        };
        const ResolvedRenderPipeline resolved =
            RenderPipelineRecipeResolver::resolve(registry, recipe, RenderDeviceCapabilities{});
        requireThrows<std::runtime_error>(
            [&] {
                RenderPipelineInstance instance{registry, resolved, FeatureCreateContext{.frameSlotCount = 2}};
            },
            "Initialization failures must escape candidate PipelineInstance creation.");
        require(probe.events == std::vector<std::string>{"factory:producer", "initialize:producer", "factory:failing",
                                                         "initialize:failing", "shutdown:failing", "shutdown:producer"},
                "Failed candidates must shut down all entered Features in reverse order.");
    }

    void testFrameTransactionsExtentAndShutdownOrder() {
        LifecycleProbe probe;
        RenderFeatureRegistry registry;
        const FrameDataContract output = FrameDataContract::of<ProducerOutput>("producer-output");

        FeatureDescriptor consumer{FeatureId{"consumer"}};
        consumer.requiredInputs = {output};
        registerLifecycleFeature(registry, std::move(consumer), probe, false, false, true);
        FeatureDescriptor producer{FeatureId{"producer"}};
        producer.outputs = {output};
        producer.historyDomains = {HistoryDomain::Taa};
        registerLifecycleFeature(registry, std::move(producer), probe, false, true, false);

        const RenderPipelineRecipe recipe{
            .id = "lifecycle",
            .features = {FeatureId{"consumer"}, FeatureId{"producer"}},
        };
        const ResolvedRenderPipeline resolved =
            RenderPipelineRecipeResolver::resolve(registry, recipe, RenderDeviceCapabilities{});
        RenderPipelineInstance instance{registry, resolved, FeatureCreateContext{.frameSlotCount = 2}};
        require(instance.features().size() == 2 && instance.features()[0]->descriptor().id == FeatureId{"producer"},
                "PipelineInstance must instantiate Features in resolved data-flow order.");

        instance.onRenderExtentChanged({1280, 720});
        FrameGraph frameGraph;
        RenderBlackboard blackboard;
        instance.prepareFrame(frameIdentity(1), 0, {}, frameGraph, blackboard);
        instance.discardFrame();
        blackboard.clear();
        instance.prepareFrame(frameIdentity(2), 0, {}, frameGraph, blackboard);
        instance.commitFrame(frameIdentity(2));
        require(instance.historyState(HistoryDomain::Taa).acceptedFrameCount == 1,
                "Only committed frames may advance PipelineInstance history.");
        instance.shutdown();

        const std::vector<std::string> expected{
            "factory:producer",   "initialize:producer", "factory:consumer",  "initialize:consumer",
            "extent:consumer",    "extent:producer",     "prepare:producer",  "prepare:consumer",
            "discarded:consumer", "discarded:producer",  "prepare:producer",  "prepare:consumer",
            "submitted:producer", "submitted:consumer",  "shutdown:consumer", "shutdown:producer",
        };
        require(probe.events == expected,
                "Extent, discard, submit, and shutdown callbacks must preserve their lifecycle ordering.");
        require(!instance.isUsable(), "A shut down PipelineInstance must reject future work.");
    }

} // namespace

int main() {
    try {
        testInitializationFailureRollsBackInReverseOrder();
        testFrameTransactionsExtentAndShutdownOrder();
        std::cout << "Render PipelineInstance tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Render PipelineInstance test failed: " << exception.what() << '\n';
        return 1;
    }
}

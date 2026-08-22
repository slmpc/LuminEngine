#include "render/core/RenderFeatureRegistry.hpp"
#include "render/core/RenderSettingsStore.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

    using namespace lumin::render::core;

    struct SceneInput {};
    struct SurfaceOutput {};
    struct LightingOutput {};

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

    class StubFeature final : public IRenderFeature {
    public:
        explicit StubFeature(FeatureDescriptor descriptor) : descriptor_(std::move(descriptor)) {
        }

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        void addPasses(RenderFeatureFrameContext&) override {
        }

    private:
        FeatureDescriptor descriptor_;
    };

    void registerStub(RenderFeatureRegistry& registry, FeatureDescriptor descriptor) {
        FeatureDescriptor factoryDescriptor = descriptor;
        registry.registerFeature(std::move(descriptor),
                                 [descriptor = std::move(factoryDescriptor)](const FeatureCreateContext&) mutable {
                                     return std::make_unique<StubFeature>(descriptor);
                                 });
    }

    void testRegistryAndDataContractsResolveDeterministically() {
        const FrameDataContract scene = FrameDataContract::of<SceneInput>("scene");
        const FrameDataContract surface = FrameDataContract::of<SurfaceOutput>("surface");
        const FrameDataContract lighting = FrameDataContract::of<LightingOutput>("lighting");

        RenderFeatureRegistry registry;
        FeatureDescriptor composite{FeatureId{"composite"}};
        composite.requiredInputs = {surface};
        composite.outputs = {lighting};
        registerStub(registry, std::move(composite));

        FeatureDescriptor surfaceFeature{FeatureId{"surface"}};
        surfaceFeature.requiredInputs = {scene};
        surfaceFeature.outputs = {surface};
        registerStub(registry, std::move(surfaceFeature));

        const RenderPipelineRecipe recipe{
            .id = "test-pipeline",
            .features = {FeatureId{"composite"}, FeatureId{"surface"}},
            .externalInputs = {scene},
        };
        const ResolvedRenderPipeline resolved =
            RenderPipelineRecipeResolver::resolve(registry, recipe, RenderDeviceCapabilities{});
        const std::vector expectedOrder = {FeatureId{"surface"}, FeatureId{"composite"}};
        require(std::ranges::equal(resolved.executionOrder(), expectedOrder),
                "Data producers must execute before consumers regardless of registration or recipe order.");
        require(registry.create(FeatureId{"surface"}, FeatureCreateContext{})->descriptor().id == FeatureId{"surface"},
                "Registered factories must create their declared Feature.");

        requireThrows<std::invalid_argument>(
            [&] {
                FeatureDescriptor duplicate{FeatureId{"surface"}};
                registerStub(registry, std::move(duplicate));
            },
            "Duplicate Feature registrations must be rejected.");
    }

    void testCapabilitiesAndMissingInputsCascade() {
        const FrameDataContract surface = FrameDataContract::of<SurfaceOutput>("surface");
        RenderFeatureRegistry registry;

        FeatureDescriptor raySurface{FeatureId{"ray-surface"}};
        raySurface.requiredCapabilities = {RenderCapability::RayTracingPipeline};
        raySurface.outputs = {surface};
        registerStub(registry, std::move(raySurface));

        FeatureDescriptor lighting{FeatureId{"lighting"}};
        lighting.requiredInputs = {surface};
        registerStub(registry, std::move(lighting));

        const RenderPipelineRecipe recipe{
            .id = "fallback",
            .features = {FeatureId{"lighting"}, FeatureId{"ray-surface"}},
        };
        const ResolvedRenderPipeline resolved =
            RenderPipelineRecipeResolver::resolve(registry, recipe, RenderDeviceCapabilities{});
        require(resolved.executionOrder().empty(), "Missing producer capabilities must disable dependent consumers.");
        require(resolved.find(FeatureId{"ray-surface"})->activation ==
                    RecipeFeatureActivation::DisabledMissingCapabilities,
                "Capability diagnostics must identify the disabled producer.");
        require(resolved.find(FeatureId{"lighting"})->activation == RecipeFeatureActivation::DisabledInput,
                "Missing typed input diagnostics must identify the disabled consumer.");

        RenderFeatureRegistry strictRegistry;
        FeatureDescriptor strict{FeatureId{"strict"}};
        strict.requiredInputs = {surface};
        strict.missingRequirementPolicy = MissingRequirementPolicy::RejectPlan;
        registerStub(strictRegistry, std::move(strict));
        requireThrows<std::runtime_error>(
            [&] {
                static_cast<void>(RenderPipelineRecipeResolver::resolve(
                    strictRegistry, RenderPipelineRecipe{.id = "strict", .features = {FeatureId{"strict"}}},
                    RenderDeviceCapabilities{}));
            },
            "Strict Features must reject recipes with unavailable typed inputs.");
    }

    struct TestSettings {
        int quality = 1;
        bool alternatePipeline = false;

        friend bool operator==(const TestSettings&, const TestSettings&) = default;
    };

    void testTypedSettingsSnapshotsAndDiffs() {
        const FeatureId feature{"settings-owner"};
        RenderSettingsSchemaRegistry schemas;
        schemas.registerSchema<TestSettings>(
            feature, TestSettings{},
            [](const TestSettings& settings) {
                if (settings.quality < 0) {
                    throw std::invalid_argument("quality");
                }
            },
            [](const TestSettings& before, const TestSettings& after) {
                FeatureSettingsChange change;
                if (before.quality != after.quality) {
                    change.impact |= SettingsChangeImpact::HistoryReset;
                    change.historyReasons.add(HistoryReason::FeatureConfigurationChanged);
                }
                if (before.alternatePipeline != after.alternatePipeline) {
                    change.impact |= SettingsChangeImpact::PipelineRecompose;
                }
                return change;
            });

        RenderSettingsStore store{schemas};
        const RenderSettingsSnapshot before = store.snapshot();
        store.set(feature, TestSettings{.quality = 3, .alternatePipeline = true});
        const RenderSettingsSnapshot after = store.snapshot();
        require(before.get<TestSettings>(feature).quality == 1,
                "A settings snapshot must not observe later store mutations.");
        require(after.get<TestSettings>(feature).quality == 3, "The next snapshot must own the updated settings.");

        const FeatureSettingsChange change = schemas.diff(before, after);
        require(hasAnyImpact(change.impact, SettingsChangeImpact::HistoryReset) &&
                    hasAnyImpact(change.impact, SettingsChangeImpact::PipelineRecompose),
                "Settings diff must combine history and pipeline impacts.");
        require(change.historyReasons.containsAny(HistoryReason::FeatureConfigurationChanged),
                "Settings diff must retain its domain-specific history reason.");
        requireThrows<std::invalid_argument>(
            [&] {
                store.set(feature, TestSettings{.quality = -1});
            },
            "Settings validation must reject invalid values before publishing them.");
    }

} // namespace

int main() {
    try {
        testRegistryAndDataContractsResolveDeterministically();
        testCapabilitiesAndMissingInputsCascade();
        testTypedSettingsSnapshotsAndDiffs();
        std::cout << "RenderModularity PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RenderModularity FAIL: " << error.what() << '\n';
        return 1;
    }
}

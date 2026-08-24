#include "render/core/FrameDataContracts.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using namespace lumin::render;

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

    class StubFeature final : public core::IRenderFeature {
    public:
        explicit StubFeature(core::FeatureDescriptor descriptor) : descriptor_(std::move(descriptor)) {
        }

        [[nodiscard]] const core::FeatureDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        void addPasses(core::RenderFeatureFrameContext&) override {
        }

    private:
        core::FeatureDescriptor descriptor_;
    };

    [[nodiscard]] core::RenderFeatureRegistry
    registryFor(const pipelines::DefaultRenderPipelineDefinition& definition) {
        core::RenderFeatureRegistry registry;
        for (const core::FeatureDescriptor& descriptor : definition.descriptors()) {
            registry.registerFeature(descriptor, [descriptor](const core::FeatureCreateContext&) {
                return std::make_unique<StubFeature>(descriptor);
            });
        }
        return registry;
    }

    [[nodiscard]] core::RenderDeviceCapabilities rasterCapabilities() {
        return {.supported = {core::RenderCapability::Graphics, core::RenderCapability::Compute,
                              core::RenderCapability::DynamicRendering},
                .maxFramesInFlight = 2};
    }

    void testRasterRecipeUsesTypedDataDag() {
        const pipelines::DefaultRenderPipelineDefinition definition =
            pipelines::makeDefaultRenderPipeline(pipelines::DefaultRenderPipelineKind::Raster);
        const core::RenderFeatureRegistry registry = registryFor(definition);
        const core::ResolvedRenderPipeline resolved =
            core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), rasterCapabilities());

        const std::vector expected = {
            pipelines::feature_ids::atmosphere(),    pipelines::feature_ids::shadow(),
            pipelines::feature_ids::rasterSurface(), pipelines::feature_ids::globalIllumination(),
            pipelines::feature_ids::denoising(),     pipelines::feature_ids::lightingComposite(),
            pipelines::feature_ids::temporalAa(),    pipelines::feature_ids::bloom(),
            pipelines::feature_ids::toneMapping(),   pipelines::feature_ids::presentation(),
        };
        require(std::ranges::equal(resolved.executionOrder(), expected),
                "Raster recipe must be ordered by typed producer/consumer contracts.");
        require(definition.descriptor(pipelines::feature_ids::rasterSurface()).outputs ==
                    std::vector{core::frame_data::rasterSurface()},
                "Raster surface Feature must publish the typed RasterSurfaceData contract.");
        require(definition.descriptor(pipelines::feature_ids::bloom()).outputs ==
                        std::vector{core::frame_data::bloomOutput()} &&
                    definition.descriptor(pipelines::feature_ids::toneMapping()).requiredInputs ==
                        std::vector{core::frame_data::scene(), core::frame_data::bloomOutput()},
                "Bloom must publish the HDR input consumed by Tone Mapping.");
        require(!registry.contains(pipelines::feature_ids::hybridSurface()),
                "Raster registration must not contain the Hybrid surface module.");
    }

    void testHybridCapabilityGateAndTopology() {
        const pipelines::DefaultRenderPipelineDefinition definition =
            pipelines::makeDefaultRenderPipeline(pipelines::DefaultRenderPipelineKind::Hybrid);
        const core::RenderFeatureRegistry registry = registryFor(definition);
        requireThrows<std::runtime_error>(
            [&] {
                static_cast<void>(
                    core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), rasterCapabilities()));
            },
            "Hybrid recipe must be rejected when acceleration-structure capabilities are unavailable.");

        core::RenderDeviceCapabilities capabilities = rasterCapabilities();
        capabilities.supported.add(core::RenderCapability::AccelerationStructure)
            .add(core::RenderCapability::RayTracingPipeline);
        const core::ResolvedRenderPipeline resolved =
            core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), capabilities);
        require(std::ranges::find(resolved.executionOrder(), pipelines::feature_ids::hybridSurface()) !=
                    resolved.executionOrder().end(),
                "Hybrid recipe must activate its RT surface producer when capabilities are present.");
        require(!registry.contains(pipelines::feature_ids::shadow()) &&
                    !registry.contains(pipelines::feature_ids::rasterSurface()),
                "Hybrid registration must not instantiate Raster-only modules.");
        require(definition.descriptor(pipelines::feature_ids::hybridSurface()).outputs ==
                    std::vector{core::frame_data::gpuScene(), core::frame_data::rtSurface()},
                "Hybrid surface Feature must publish GPU Scene and RT surface contracts.");
    }

    void testDefaultSettingsSchemasClassifyChanges() {
        core::RenderSettingsSchemaRegistry schemas;
        pipelines::registerDefaultRenderSettings(schemas);
        core::RenderSettingsStore store{schemas};
        const core::RenderSettingsSnapshot defaults = store.snapshot();
        require(schemas.diff(defaults, defaults).impact == core::SettingsChangeImpact::None,
                "Unchanged default settings must not schedule runtime work.");

        ToneMappingSettings toneMapping = defaults.get<ToneMappingSettings>(pipelines::feature_ids::toneMapping());
        toneMapping.exposure = 1.5f;
        toneMapping.exposureCompensationEv = 1.0f;
        store.set(pipelines::feature_ids::toneMapping(), toneMapping);
        const core::FeatureSettingsChange hotUpdate = schemas.diff(defaults, store.snapshot());
        require(core::hasAnyImpact(hotUpdate.impact, core::SettingsChangeImpact::HotUpdate) &&
                    !core::hasAnyImpact(hotUpdate.impact, core::SettingsChangeImpact::PipelineRecompose),
                "Exposure changes must remain hot updates.");

        core::RenderSettingsStore bloomStore{schemas};
        BloomSettings bloom = defaults.get<BloomSettings>(pipelines::feature_ids::bloom());
        bloom.enabled = false;
        bloom.intensity = 0.2f;
        bloomStore.set(pipelines::feature_ids::bloom(), bloom);
        const core::FeatureSettingsChange bloomChange = schemas.diff(defaults, bloomStore.snapshot());
        require(core::hasAnyImpact(bloomChange.impact, core::SettingsChangeImpact::HotUpdate) &&
                    bloomChange.historyReasons.empty(),
                "Bloom controls must update after TAA without invalidating temporal histories.");

        core::RenderSettingsStore sharpnessStore{schemas};
        TemporalAaSettings temporalAa = defaults.get<TemporalAaSettings>(pipelines::feature_ids::temporalAa());
        temporalAa.sharpness = 0.75f;
        sharpnessStore.set(pipelines::feature_ids::temporalAa(), temporalAa);
        const core::FeatureSettingsChange sharpnessChange = schemas.diff(defaults, sharpnessStore.snapshot());
        require(core::hasAnyImpact(sharpnessChange.impact, core::SettingsChangeImpact::HotUpdate) &&
                    sharpnessChange.historyReasons.empty(),
                "RCAS sharpness changes must not invalidate the unsharpened TAA history.");

        core::RenderSettingsStore topologyStore{schemas};
        GlobalIlluminationSettings gi =
            defaults.get<GlobalIlluminationSettings>(pipelines::feature_ids::globalIllumination());
        gi.mode = gi.mode == GlobalIlluminationMode::Legacy ? GlobalIlluminationMode::RayTracing
                                                            : GlobalIlluminationMode::Legacy;
        topologyStore.set(pipelines::feature_ids::globalIllumination(), gi);
        const core::FeatureSettingsChange topologyChange = schemas.diff(defaults, topologyStore.snapshot());
        require(core::hasAnyImpact(topologyChange.impact, core::SettingsChangeImpact::PipelineRecompose) &&
                    topologyChange.historyReasons.containsAny(core::HistoryReason::FeatureConfigurationChanged),
                "GI topology changes must request recipe recomposition and history invalidation.");

        core::RenderSettingsStore sharcStore{schemas};
        GlobalIlluminationSettings sharcSettings =
            defaults.get<GlobalIlluminationSettings>(pipelines::feature_ids::globalIllumination());
        sharcSettings.sharcEnabled = false;
        sharcSettings.nrdEnabled = false;
        sharcStore.set(pipelines::feature_ids::globalIllumination(), sharcSettings);
        const core::FeatureSettingsChange sharcChange = schemas.diff(defaults, sharcStore.snapshot());
        require(core::hasAnyImpact(sharcChange.impact, core::SettingsChangeImpact::HistoryReset) &&
                    !core::hasAnyImpact(sharcChange.impact, core::SettingsChangeImpact::PipelineRecompose) &&
                    sharcChange.historyReasons.containsAny(core::HistoryReason::FeatureConfigurationChanged),
                "SHARC toggles must reset histories without recomposing the resident Hybrid recipe.");

        requireThrows<std::invalid_argument>(
            [&] {
                ToneMappingSettings invalid;
                invalid.exposure = -1.0f;
                topologyStore.set(pipelines::feature_ids::toneMapping(), invalid);
            },
            "Default settings validators must reject invalid public values.");
        requireThrows<std::invalid_argument>(
            [&] {
                ToneMappingSettings invalid;
                invalid.minimumExposureEv = invalid.maximumExposureEv;
                topologyStore.set(pipelines::feature_ids::toneMapping(), invalid);
            },
            "Auto exposure must reject an empty EV range.");
        requireThrows<std::invalid_argument>(
            [&] {
                TemporalAaSettings invalid;
                invalid.sharpness = 1.1f;
                topologyStore.set(pipelines::feature_ids::temporalAa(), invalid);
            },
            "Temporal AA settings must reject RCAS sharpness outside [0, 1].");
        requireThrows<std::invalid_argument>(
            [&] {
                BloomSettings invalid;
                invalid.radius = 4.5f;
                topologyStore.set(pipelines::feature_ids::bloom(), invalid);
            },
            "Bloom settings must reject a radius outside the supported filter range.");
    }

} // namespace

int main() {
    try {
        testRasterRecipeUsesTypedDataDag();
        testHybridCapabilityGateAndTopology();
        testDefaultSettingsSchemasClassifyChanges();
        std::cout << "Default render pipeline tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Default render pipeline test failed: " << exception.what() << '\n';
        return 1;
    }
}

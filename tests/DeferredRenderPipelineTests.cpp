#include "render/DeferredRenderPipeline.hpp"
#include "render/features/LevelRenderFeature.hpp"

#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
        bool thrown = false;
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            thrown = true;
        }
        require(thrown, message);
    }

    [[nodiscard]] std::string kindName(LevelRenderFeatureKind kind) {
        return deferredFeatureDescriptor(kind, DeferredRenderPath::Raster).id.value();
    }

    struct HostProbe final : LevelRenderFeatureHost {
        std::vector<std::string> events;
        std::optional<LevelRenderFeatureKind> failingKind;

        void addFeaturePasses(LevelRenderFeatureKind kind, RenderFeatureFrameContext&) override {
            events.emplace_back(std::string{"prepare:"} + kindName(kind));
            if (failingKind == kind) {
                throw std::runtime_error("host prepare failure");
            }
        }

        void submitFeature(LevelRenderFeatureKind kind, const RenderFrameIdentity&) noexcept override {
            events.emplace_back(std::string{"submit:"} + kindName(kind));
        }

        void discardFeature(LevelRenderFeatureKind kind, const RenderFrameIdentity&) noexcept override {
            events.emplace_back(std::string{"discard:"} + kindName(kind));
        }
    };

    [[nodiscard]] std::unique_ptr<IRenderFeature> makeFeature(LevelRenderFeatureKind kind,
                                                               DeferredRenderPath path, HostProbe& host) {
        return makeLevelRenderFeature(kind, deferredFeatureDescriptor(kind, path), host);
    }

    [[nodiscard]] DeferredRenderFeatureSet makeFeatureSet(DeferredRenderPath path, HostProbe& host) {
        DeferredRenderFeatureSet features;
        if (path == DeferredRenderPath::Raster) {
            features.shadow = makeFeature(LevelRenderFeatureKind::Shadow, path, host);
            features.gbuffer = makeFeature(LevelRenderFeatureKind::GBuffer, path, host);
        } else {
            features.hybridSurface = makeFeature(LevelRenderFeatureKind::HybridSurface, path, host);
        }
        features.atmosphereLuts = makeFeature(LevelRenderFeatureKind::AtmosphereLuts, path, host);
        features.globalIllumination = makeFeature(LevelRenderFeatureKind::GlobalIllumination, path, host);
        features.giDenoiser = makeFeature(LevelRenderFeatureKind::GiDenoiser, path, host);
        features.skyComposite = makeFeature(LevelRenderFeatureKind::SkyComposite, path, host);
        features.directLighting = makeFeature(LevelRenderFeatureKind::DirectLighting, path, host);
        features.temporalAa = makeFeature(LevelRenderFeatureKind::TemporalAa, path, host);
        features.toneMapping = makeFeature(LevelRenderFeatureKind::ToneMapping, path, host);
        features.uiPresent = makeFeature(LevelRenderFeatureKind::UiPresent, path, host);
        return features;
    }

    [[nodiscard]] RenderDeviceCapabilities rasterCapabilities() {
        return RenderDeviceCapabilities{.supported = {RenderCapability::Graphics, RenderCapability::DynamicRendering},
                                        .maxFramesInFlight = 2};
    }

    [[nodiscard]] RenderDeviceCapabilities hybridCapabilities() {
        RenderDeviceCapabilities capabilities = rasterCapabilities();
        capabilities.supported.add(RenderCapability::AccelerationStructure)
            .add(RenderCapability::RayTracingPipeline);
        return capabilities;
    }

    [[nodiscard]] RenderFrameIdentity testIdentity(std::uint64_t sequence) {
        return {FrameSlotIndex{0}, SwapImageIndex{0}, RenderSequence{sequence}, {1280, 720}};
    }

    void requireDependencies(const FeatureDescriptor& descriptor,
                             std::initializer_list<std::string_view> expected) {
        require(descriptor.dependencies.size() == expected.size(),
                "Feature descriptor dependency count changed unexpectedly.");
        auto expectedIterator = expected.begin();
        for (const FeatureId& dependency : descriptor.dependencies) {
            require(dependency.value() == *expectedIterator++,
                    "Feature descriptor dependency order changed unexpectedly.");
        }
    }

    void requireHistoryDomains(const FeatureDescriptor& descriptor,
                               std::initializer_list<HistoryDomain> expected) {
        require(descriptor.historyDomains.size() == expected.size(),
                "Feature descriptor history ownership changed unexpectedly.");
        auto expectedIterator = expected.begin();
        for (const HistoryDomain domain : descriptor.historyDomains) {
            require(domain == *expectedIterator++, "Feature descriptor history domain changed unexpectedly.");
        }
    }

    void testDescriptorContracts() {
        const FeatureDescriptor shadow = deferredFeatureDescriptor(LevelRenderFeatureKind::Shadow,
                                                                    DeferredRenderPath::Raster);
        require(shadow.id.value() == "shadow", "Shadow Feature id must be stable.");
        requireDependencies(shadow, {});
        requireHistoryDomains(shadow, {});

        const FeatureDescriptor gbuffer = deferredFeatureDescriptor(LevelRenderFeatureKind::GBuffer,
                                                                    DeferredRenderPath::Raster);
        require(gbuffer.id.value() == "gbuffer", "G-buffer Feature id must be stable.");
        requireDependencies(gbuffer, {"shadow"});

        const FeatureDescriptor atmosphere = deferredFeatureDescriptor(LevelRenderFeatureKind::AtmosphereLuts,
                                                                        DeferredRenderPath::Raster);
        requireDependencies(atmosphere, {"gbuffer"});
        requireHistoryDomains(atmosphere, {HistoryDomain::AtmosphereLut});

        const FeatureDescriptor globalIllumination =
            deferredFeatureDescriptor(LevelRenderFeatureKind::GlobalIllumination, DeferredRenderPath::Raster);
        requireDependencies(globalIllumination, {"atmosphere-luts"});
        requireHistoryDomains(globalIllumination, {HistoryDomain::Sharc});

        const FeatureDescriptor denoiser = deferredFeatureDescriptor(LevelRenderFeatureKind::GiDenoiser,
                                                                     DeferredRenderPath::Raster);
        requireDependencies(denoiser, {"global-illumination"});
        requireHistoryDomains(denoiser, {HistoryDomain::NrdDiffuse, HistoryDomain::NrdSpecular});

        const FeatureDescriptor sky = deferredFeatureDescriptor(LevelRenderFeatureKind::SkyComposite,
                                                                 DeferredRenderPath::Raster);
        requireDependencies(sky, {"gi-denoiser", "atmosphere-luts"});

        const FeatureDescriptor rasterLighting =
            deferredFeatureDescriptor(LevelRenderFeatureKind::DirectLighting, DeferredRenderPath::Raster);
        requireDependencies(rasterLighting,
                             {"global-illumination", "gi-denoiser", "sky-composite", "shadow", "gbuffer"});

        const FeatureDescriptor hybridSurface =
            deferredFeatureDescriptor(LevelRenderFeatureKind::HybridSurface, DeferredRenderPath::Hybrid);
        require(hybridSurface.id.value() == "hybrid-surface", "Hybrid surface Feature id must be stable.");
        requireDependencies(hybridSurface, {"atmosphere-luts"});
        require(hybridSurface.requiredCapabilities.contains(RenderCapability::AccelerationStructure) &&
                    hybridSurface.requiredCapabilities.contains(RenderCapability::RayTracingPipeline),
                "Hybrid surface must require acceleration structures and RT pipeline support.");
        require(hybridSurface.missingRequirementPolicy == MissingRequirementPolicy::RejectPlan,
                "Hybrid surface capability failure must reject the plan.");

        const FeatureDescriptor hybridAtmosphere =
            deferredFeatureDescriptor(LevelRenderFeatureKind::AtmosphereLuts, DeferredRenderPath::Hybrid);
        requireDependencies(hybridAtmosphere, {});

        const FeatureDescriptor hybridLighting =
            deferredFeatureDescriptor(LevelRenderFeatureKind::DirectLighting, DeferredRenderPath::Hybrid);
        requireDependencies(hybridLighting, {"global-illumination", "gi-denoiser", "sky-composite"});

        std::cout << "FEATURE_DESCRIPTORS=ids+dependencies+history-domains\n";
    }

    void testPathRegistrationConstraints() {
        HostProbe host;
        const RenderDeviceCapabilities raster = rasterCapabilities();
        const RenderDeviceCapabilities hybrid = hybridCapabilities();

        DeferredRenderPipeline rasterPipeline(makeFeatureSet(DeferredRenderPath::Raster, host), raster,
                                              DeferredRenderPath::Raster);
        require(rasterPipeline.path() == DeferredRenderPath::Raster, "Raster pipeline path must be retained.");
        require(rasterPipeline.resolvedPlan().executionOrder().size() == 10,
                "Raster pipeline must resolve all ten raster Features.");

        DeferredRenderPipeline hybridPipeline(makeFeatureSet(DeferredRenderPath::Hybrid, host), hybrid,
                                              DeferredRenderPath::Hybrid);
        require(hybridPipeline.path() == DeferredRenderPath::Hybrid, "Hybrid pipeline path must be retained.");
        require(hybridPipeline.resolvedPlan().executionOrder().size() == 9,
                "Hybrid pipeline must replace shadow and G-buffer with the surface Feature.");

        DeferredRenderFeatureSet rasterWithHybrid = makeFeatureSet(DeferredRenderPath::Raster, host);
        rasterWithHybrid.hybridSurface = makeFeature(LevelRenderFeatureKind::HybridSurface,
                                                     DeferredRenderPath::Hybrid, host);
        requireThrows<std::invalid_argument>(
            [&] {
                DeferredRenderPipeline invalid(std::move(rasterWithHybrid), raster, DeferredRenderPath::Raster);
            },
            "Raster pipeline must reject a Hybrid-only Feature.");

        DeferredRenderFeatureSet hybridWithRaster = makeFeatureSet(DeferredRenderPath::Hybrid, host);
        hybridWithRaster.gbuffer = makeFeature(LevelRenderFeatureKind::GBuffer, DeferredRenderPath::Raster, host);
        requireThrows<std::invalid_argument>(
            [&] {
                DeferredRenderPipeline invalid(std::move(hybridWithRaster), hybrid, DeferredRenderPath::Hybrid);
            },
            "Hybrid pipeline must reject raster-only Features.");

        DeferredRenderFeatureSet hybridWithoutSurface = makeFeatureSet(DeferredRenderPath::Hybrid, host);
        hybridWithoutSurface.hybridSurface.reset();
        requireThrows<std::invalid_argument>(
            [&] {
                DeferredRenderPipeline invalid(std::move(hybridWithoutSurface), hybrid, DeferredRenderPath::Hybrid);
            },
            "Hybrid pipeline must require its primary surface Feature.");

        DeferredRenderFeatureSet wrongId = makeFeatureSet(DeferredRenderPath::Raster, host);
        wrongId.shadow = makeFeature(LevelRenderFeatureKind::GBuffer, DeferredRenderPath::Raster, host);
        requireThrows<std::invalid_argument>(
            [&] {
                DeferredRenderPipeline invalid(std::move(wrongId), raster, DeferredRenderPath::Raster);
            },
            "Pipeline registration must reject a Feature with the wrong descriptor id.");

        DeferredRenderFeatureSet wrongDependencies = makeFeatureSet(DeferredRenderPath::Raster, host);
        FeatureDescriptor missingDependency =
            deferredFeatureDescriptor(LevelRenderFeatureKind::GBuffer, DeferredRenderPath::Raster);
        missingDependency.dependencies.clear();
        wrongDependencies.gbuffer = makeLevelRenderFeature(LevelRenderFeatureKind::GBuffer,
                                                            std::move(missingDependency), host);
        requireThrows<std::invalid_argument>(
            [&] {
                DeferredRenderPipeline invalid(std::move(wrongDependencies), raster, DeferredRenderPath::Raster);
            },
            "Pipeline registration must reject a Feature with incompatible dependencies.");

        DeferredRenderFeatureSet wrongHistory = makeFeatureSet(DeferredRenderPath::Raster, host);
        FeatureDescriptor missingHistory =
            deferredFeatureDescriptor(LevelRenderFeatureKind::TemporalAa, DeferredRenderPath::Raster);
        missingHistory.historyDomains.clear();
        wrongHistory.temporalAa = makeLevelRenderFeature(LevelRenderFeatureKind::TemporalAa,
                                                         std::move(missingHistory), host);
        requireThrows<std::invalid_argument>(
            [&] {
                DeferredRenderPipeline invalid(std::move(wrongHistory), raster, DeferredRenderPath::Raster);
            },
            "Pipeline registration must reject a Feature with incompatible history ownership.");

        std::cout << "FEATURE_REGISTRATION=Raster/Hybrid constraints\n";
    }

    void testFeatureHostTransactionOrder() {
        const RenderDeviceCapabilities raster = rasterCapabilities();
        const RenderFrameIdentity identity = testIdentity(1);

        HostProbe commitHost;
        DeferredRenderPipeline commitPipeline(makeFeatureSet(DeferredRenderPath::Raster, commitHost), raster,
                                              DeferredRenderPath::Raster);
        FrameGraph frameGraph;
        RenderBlackboard blackboard;
        commitPipeline.prepareFrame(identity, 0, FrameChangeSet{}, frameGraph, blackboard);
        commitPipeline.commitFrame(identity);

        const std::vector<std::string> expectedPrefix = {
            "prepare:shadow", "prepare:gbuffer", "prepare:atmosphere-luts", "prepare:global-illumination",
            "prepare:gi-denoiser", "prepare:sky-composite", "prepare:direct-lighting", "prepare:temporal-aa",
            "prepare:tone-mapping", "prepare:ui-present", "submit:shadow", "submit:gbuffer",
            "submit:atmosphere-luts", "submit:global-illumination", "submit:gi-denoiser", "submit:sky-composite",
            "submit:direct-lighting", "submit:temporal-aa", "submit:tone-mapping", "submit:ui-present"};
        require(commitHost.events == expectedPrefix,
                "Feature host submit callbacks must follow the resolved dependency order.");
        require(commitPipeline.historyState(HistoryDomain::Taa).acceptedFrameCount == 1 &&
                    commitPipeline.historyState(HistoryDomain::AtmosphereLut).acceptedFrameCount == 1 &&
                    commitPipeline.historyState(HistoryDomain::Sharc).acceptedFrameCount == 1,
                "Feature-owned history domains must commit exactly once.");

        HostProbe discardHost;
        DeferredRenderPipeline discardPipeline(makeFeatureSet(DeferredRenderPath::Raster, discardHost), raster,
                                               DeferredRenderPath::Raster);
        discardPipeline.prepareFrame(testIdentity(2), 0, FrameChangeSet{}, frameGraph, blackboard);
        discardPipeline.discardFrame();
        const std::vector<std::string> expectedDiscard = {
            "prepare:shadow", "prepare:gbuffer", "prepare:atmosphere-luts", "prepare:global-illumination",
            "prepare:gi-denoiser", "prepare:sky-composite", "prepare:direct-lighting", "prepare:temporal-aa",
            "prepare:tone-mapping", "prepare:ui-present", "discard:ui-present", "discard:tone-mapping",
            "discard:temporal-aa", "discard:direct-lighting", "discard:sky-composite", "discard:gi-denoiser",
            "discard:global-illumination", "discard:atmosphere-luts", "discard:gbuffer", "discard:shadow"};
        require(discardHost.events == expectedDiscard,
                "Feature host discard callbacks must run in reverse prepare order.");
        require(discardPipeline.historyState(HistoryDomain::Taa).acceptedFrameCount == 0,
                "Discarded Feature frames must not commit history.");

        HostProbe failingHost;
        failingHost.failingKind = LevelRenderFeatureKind::SkyComposite;
        DeferredRenderPipeline failingPipeline(makeFeatureSet(DeferredRenderPath::Raster, failingHost), raster,
                                               DeferredRenderPath::Raster);
        requireThrows<std::runtime_error>(
            [&] {
                failingPipeline.prepareFrame(testIdentity(3), 0, FrameChangeSet{}, frameGraph, blackboard);
            },
            "A host preparation failure must propagate through the pipeline.");
        const std::vector<std::string> expectedFailure = {
            "prepare:shadow", "prepare:gbuffer", "prepare:atmosphere-luts", "prepare:global-illumination",
            "prepare:gi-denoiser", "prepare:sky-composite", "discard:sky-composite", "discard:gi-denoiser",
            "discard:global-illumination", "discard:atmosphere-luts", "discard:gbuffer", "discard:shadow"};
        require(failingHost.events == expectedFailure,
                "Preparation failure must discard all entered Features in reverse order.");
        require(!failingPipeline.hasPendingFrame(), "Failed Feature preparation must close the transaction.");

        std::cout << "FEATURE_TRANSACTIONS=submit-forward/discard-reverse\n";
    }

} // namespace

int main() {
    try {
        testDescriptorContracts();
        testPathRegistrationConstraints();
        testFeatureHostTransactionOrder();
        std::cout << "Deferred render pipeline tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Deferred render pipeline test failed: " << exception.what() << '\n';
        return 1;
    }
}

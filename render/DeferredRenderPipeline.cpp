#include "render/DeferredRenderPipeline.hpp"

#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lumin::render {

    namespace {

        [[nodiscard]] bool descriptorMatches(const core::FeatureDescriptor& actual,
                                              const core::FeatureDescriptor& expected) noexcept {
            return actual.id == expected.id && actual.requiredCapabilities == expected.requiredCapabilities &&
                   actual.optionalCapabilities == expected.optionalCapabilities &&
                   actual.dependencies == expected.dependencies &&
                   actual.missingRequirementPolicy == expected.missingRequirementPolicy &&
                   actual.historyDomains == expected.historyDomains;
        }

        [[nodiscard]] core::FeatureDescriptor feature(std::string_view id,
                                                      std::initializer_list<std::string_view> dependencies = {},
                                                      std::initializer_list<core::HistoryDomain> histories = {}) {
            core::FeatureDescriptor descriptor{core::FeatureId{id}};
            descriptor.requiredCapabilities = {core::RenderCapability::Graphics,
                                               core::RenderCapability::DynamicRendering};
            for (const std::string_view dependency : dependencies) {
                descriptor.dependencies.emplace_back(dependency);
            }
            descriptor.historyDomains.assign(histories);
            return descriptor;
        }

    } // namespace

    core::FeatureDescriptor deferredFeatureDescriptor(LevelRenderFeatureKind kind, DeferredRenderPath path) {
        switch (kind) {
        case LevelRenderFeatureKind::Shadow:
            return feature("shadow");
        case LevelRenderFeatureKind::GBuffer:
            return feature("gbuffer", {"shadow"});
        case LevelRenderFeatureKind::HybridSurface: {
            core::FeatureDescriptor descriptor = feature("hybrid-surface", {"atmosphere-luts"});
            descriptor.requiredCapabilities.add(core::RenderCapability::AccelerationStructure)
                .add(core::RenderCapability::RayTracingPipeline);
            descriptor.missingRequirementPolicy = core::MissingRequirementPolicy::RejectPlan;
            return descriptor;
        }
        case LevelRenderFeatureKind::AtmosphereLuts:
            return feature("atmosphere-luts",
                           path == DeferredRenderPath::Raster
                               ? std::initializer_list<std::string_view>{"gbuffer"}
                               : std::initializer_list<std::string_view>{},
                           {core::HistoryDomain::AtmosphereLut});
        case LevelRenderFeatureKind::GlobalIllumination:
            return feature("global-illumination",
                           path == DeferredRenderPath::Raster
                               ? std::initializer_list<std::string_view>{"atmosphere-luts"}
                               : std::initializer_list<std::string_view>{"hybrid-surface"},
                           {core::HistoryDomain::Sharc});
        case LevelRenderFeatureKind::GiDenoiser:
            return feature("gi-denoiser", {"global-illumination"},
                           {core::HistoryDomain::NrdDiffuse, core::HistoryDomain::NrdSpecular});
        case LevelRenderFeatureKind::SkyComposite:
            return feature("sky-composite", {"gi-denoiser", "atmosphere-luts"});
        case LevelRenderFeatureKind::DirectLighting: {
            core::FeatureDescriptor descriptor =
                feature("direct-lighting", {"global-illumination", "gi-denoiser", "sky-composite"});
            if (path == DeferredRenderPath::Raster) {
                descriptor.dependencies.emplace_back("shadow");
                descriptor.dependencies.emplace_back("gbuffer");
            }
            return descriptor;
        }
        case LevelRenderFeatureKind::TemporalAa:
            return feature("temporal-aa", {"direct-lighting"}, {core::HistoryDomain::Taa});
        case LevelRenderFeatureKind::ToneMapping:
            return feature("tone-mapping", {"temporal-aa"});
        case LevelRenderFeatureKind::UiPresent:
            return feature("ui-present", {"tone-mapping"});
        }
        throw std::invalid_argument("Unknown deferred render feature kind.");
    }

    DeferredRenderPipeline::DeferredRenderPipeline(DeferredRenderFeatureSet features,
                                                   const core::RenderDeviceCapabilities& capabilities,
                                                   DeferredRenderPath path)
        : path_(path) {
        const auto addFeature = [this](std::unique_ptr<core::IRenderFeature> featureObject,
                                       LevelRenderFeatureKind kind) {
            if (!featureObject) {
                throw std::invalid_argument("Deferred render pipeline received a missing Feature.");
            }
            const core::FeatureDescriptor expected = deferredFeatureDescriptor(kind, path_);
            if (!descriptorMatches(featureObject->descriptor(), expected)) {
                throw std::invalid_argument(
                    "Deferred render pipeline received a Feature with an incompatible descriptor contract.");
            }
            pipeline_.addFeature(std::move(featureObject));
        };

        if (path_ == DeferredRenderPath::Raster) {
            addFeature(std::move(features.shadow), LevelRenderFeatureKind::Shadow);
            addFeature(std::move(features.gbuffer), LevelRenderFeatureKind::GBuffer);
        } else if (features.shadow || features.gbuffer) {
            throw std::invalid_argument("Hybrid deferred render pipeline cannot register raster-only Features.");
        }

        addFeature(std::move(features.atmosphereLuts), LevelRenderFeatureKind::AtmosphereLuts);
        if (path_ == DeferredRenderPath::Hybrid) {
            addFeature(std::move(features.hybridSurface), LevelRenderFeatureKind::HybridSurface);
        } else if (features.hybridSurface) {
            throw std::invalid_argument("Raster deferred render pipeline cannot register the Hybrid surface Feature.");
        }
        addFeature(std::move(features.globalIllumination), LevelRenderFeatureKind::GlobalIllumination);
        addFeature(std::move(features.giDenoiser), LevelRenderFeatureKind::GiDenoiser);
        addFeature(std::move(features.skyComposite), LevelRenderFeatureKind::SkyComposite);
        addFeature(std::move(features.directLighting), LevelRenderFeatureKind::DirectLighting);
        addFeature(std::move(features.temporalAa), LevelRenderFeatureKind::TemporalAa);
        addFeature(std::move(features.toneMapping), LevelRenderFeatureKind::ToneMapping);
        addFeature(std::move(features.uiPresent), LevelRenderFeatureKind::UiPresent);
        pipeline_.resolve(capabilities);
    }

    void DeferredRenderPipeline::prepareFrame(const core::RenderFrameIdentity& identity, std::uint64_t cameraCutEpoch,
                                              const core::FrameChangeSet& changes, FrameGraph& frameGraph,
                                              core::RenderBlackboard& blackboard) {
        pipeline_.prepareFrame(identity, cameraCutEpoch, changes, frameGraph, blackboard);
    }

    void DeferredRenderPipeline::commitFrame(const core::RenderFrameIdentity& identity) {
        pipeline_.commitFrame(identity);
    }

    void DeferredRenderPipeline::discardFrame() noexcept {
        pipeline_.discardFrame();
    }

    const core::ResolvedRenderFeaturePlan& DeferredRenderPipeline::resolvedPlan() const noexcept {
        return *pipeline_.resolvedPlan();
    }

    bool DeferredRenderPipeline::hasPendingFrame() const noexcept {
        return pipeline_.hasPendingFrame();
    }

    const core::HistoryDomainState& DeferredRenderPipeline::historyState(core::HistoryDomain domain) const {
        return pipeline_.historyState(domain);
    }

    DeferredRenderPath DeferredRenderPipeline::path() const noexcept {
        return path_;
    }

} // namespace lumin::render

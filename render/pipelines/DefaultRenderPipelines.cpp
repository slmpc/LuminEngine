#include "render/pipelines/DefaultRenderPipelines.hpp"

#include "render/core/FrameDataContracts.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace lumin::render::pipelines {
    namespace {

        [[nodiscard]] core::FeatureDescriptor graphicsFeature(const core::FeatureId& id) {
            core::FeatureDescriptor descriptor{id};
            descriptor.requiredCapabilities = {core::RenderCapability::Graphics,
                                               core::RenderCapability::DynamicRendering};
            descriptor.missingRequirementPolicy = core::MissingRequirementPolicy::RejectPlan;
            return descriptor;
        }

    } // namespace

    DefaultRenderPipelineDefinition::DefaultRenderPipelineDefinition(core::RenderPipelineRecipe recipe,
                                                                     std::vector<core::FeatureDescriptor> descriptors)
        : recipe_(std::move(recipe)), descriptors_(std::move(descriptors)) {
    }

    const core::RenderPipelineRecipe& DefaultRenderPipelineDefinition::recipe() const noexcept {
        return recipe_;
    }

    std::span<const core::FeatureDescriptor> DefaultRenderPipelineDefinition::descriptors() const noexcept {
        return descriptors_;
    }

    const core::FeatureDescriptor& DefaultRenderPipelineDefinition::descriptor(const core::FeatureId& id) const {
        const auto found = std::ranges::find(descriptors_, id, &core::FeatureDescriptor::id);
        if (found == descriptors_.end()) {
            throw std::out_of_range("Default render recipe does not contain Feature: " + id.value());
        }
        return *found;
    }

    namespace feature_ids {

        const core::FeatureId& shadow() {
            static const core::FeatureId id{"raster.shadow"};
            return id;
        }

        const core::FeatureId& rasterSurface() {
            static const core::FeatureId id{"raster.surface"};
            return id;
        }

        const core::FeatureId& hybridSurface() {
            static const core::FeatureId id{"ray-tracing.surface"};
            return id;
        }

        const core::FeatureId& atmosphere() {
            static const core::FeatureId id{"atmosphere.luts"};
            return id;
        }

        const core::FeatureId& globalIllumination() {
            static const core::FeatureId id{"global-illumination"};
            return id;
        }

        const core::FeatureId& denoising() {
            static const core::FeatureId id{"denoising"};
            return id;
        }

        const core::FeatureId& lightingComposite() {
            static const core::FeatureId id{"lighting-composite"};
            return id;
        }

        const core::FeatureId& temporalAa() {
            static const core::FeatureId id{"postfx.temporal-aa"};
            return id;
        }

        const core::FeatureId& toneMapping() {
            static const core::FeatureId id{"postfx.tone-mapping"};
            return id;
        }

        const core::FeatureId& presentation() {
            static const core::FeatureId id{"presentation"};
            return id;
        }

    } // namespace feature_ids

    DefaultRenderPipelineDefinition makeDefaultRenderPipeline(DefaultRenderPipelineKind kind) {
        using namespace core;
        using namespace feature_ids;

        std::vector<FeatureDescriptor> descriptors;
        std::vector<FeatureId> selected;
        const auto add = [&descriptors, &selected](FeatureDescriptor descriptor) {
            selected.push_back(descriptor.id);
            descriptors.push_back(std::move(descriptor));
        };

        FeatureDescriptor atmosphereFeature = graphicsFeature(atmosphere());
        atmosphereFeature.requiredInputs = {frame_data::scene()};
        atmosphereFeature.outputs = {frame_data::atmosphere()};
        atmosphereFeature.historyDomains = {HistoryDomain::AtmosphereLut};
        add(std::move(atmosphereFeature));

        if (kind == DefaultRenderPipelineKind::Raster) {
            FeatureDescriptor shadowFeature = graphicsFeature(shadow());
            shadowFeature.requiredInputs = {frame_data::scene()};
            shadowFeature.outputs = {frame_data::shadows()};
            add(std::move(shadowFeature));

            FeatureDescriptor surfaceFeature = graphicsFeature(rasterSurface());
            surfaceFeature.requiredInputs = {frame_data::scene()};
            surfaceFeature.outputs = {frame_data::rasterSurface()};
            add(std::move(surfaceFeature));
        } else {
            FeatureDescriptor surfaceFeature = graphicsFeature(hybridSurface());
            surfaceFeature.requiredCapabilities.add(RenderCapability::AccelerationStructure)
                .add(RenderCapability::RayTracingPipeline);
            surfaceFeature.requiredInputs = {frame_data::scene(), frame_data::atmosphere()};
            surfaceFeature.outputs = {frame_data::gpuScene(), frame_data::rtSurface()};
            add(std::move(surfaceFeature));
        }

        FeatureDescriptor giFeature = graphicsFeature(globalIllumination());
        giFeature.requiredInputs = {frame_data::scene(), frame_data::atmosphere()};
        if (kind == DefaultRenderPipelineKind::Raster) {
            giFeature.requiredInputs.push_back(frame_data::rasterSurface());
        } else {
            giFeature.requiredInputs.push_back(frame_data::gpuScene());
            giFeature.requiredInputs.push_back(frame_data::rtSurface());
        }
        giFeature.outputs = {frame_data::indirectLighting()};
        giFeature.historyDomains = {HistoryDomain::Sharc};
        add(std::move(giFeature));

        FeatureDescriptor denoisingFeature = graphicsFeature(denoising());
        denoisingFeature.requiredInputs = {frame_data::scene(), frame_data::indirectLighting()};
        denoisingFeature.requiredInputs.push_back(
            kind == DefaultRenderPipelineKind::Raster ? frame_data::rasterSurface() : frame_data::rtSurface());
        denoisingFeature.outputs = {frame_data::denoisedLighting()};
        denoisingFeature.historyDomains = {HistoryDomain::NrdDiffuse, HistoryDomain::NrdSpecular};
        add(std::move(denoisingFeature));

        FeatureDescriptor lightingFeature = graphicsFeature(lightingComposite());
        lightingFeature.requiredInputs = {frame_data::scene(), frame_data::atmosphere(),
                                          frame_data::denoisedLighting()};
        if (kind == DefaultRenderPipelineKind::Raster) {
            lightingFeature.requiredInputs.push_back(frame_data::shadows());
            lightingFeature.requiredInputs.push_back(frame_data::rasterSurface());
        } else {
            lightingFeature.requiredInputs.push_back(frame_data::rtSurface());
        }
        lightingFeature.outputs = {frame_data::sceneHdr()};
        add(std::move(lightingFeature));

        FeatureDescriptor temporalFeature = graphicsFeature(temporalAa());
        temporalFeature.requiredInputs = {frame_data::scene(), frame_data::sceneHdr()};
        temporalFeature.outputs = {frame_data::temporalOutput()};
        temporalFeature.historyDomains = {HistoryDomain::Taa};
        add(std::move(temporalFeature));

        FeatureDescriptor toneMappingFeature = graphicsFeature(toneMapping());
        toneMappingFeature.requiredInputs = {frame_data::scene(), frame_data::temporalOutput()};
        toneMappingFeature.outputs = {frame_data::viewportOutput()};
        add(std::move(toneMappingFeature));

        FeatureDescriptor presentationFeature = graphicsFeature(presentation());
        presentationFeature.requiredInputs = {frame_data::viewportOutput(), frame_data::presentationInput()};
        presentationFeature.outputs = {frame_data::present()};
        presentationFeature.after = {toneMapping()};
        add(std::move(presentationFeature));

        return DefaultRenderPipelineDefinition{
            RenderPipelineRecipe{
                .id = kind == DefaultRenderPipelineKind::Raster ? "raster" : "hybrid",
                .features = std::move(selected),
                .externalInputs = {frame_data::scene(), frame_data::presentationInput()},
            },
            std::move(descriptors),
        };
    }

} // namespace lumin::render::pipelines

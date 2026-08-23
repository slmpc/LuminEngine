#include "render/gpu/GpuScene.hpp"
#include "render/level/FeatureFrameData.hpp"
#include "render/pipelines/default/DefaultRenderPipelineSession.hpp"

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
#include "render/gi/raytracing/HybridLightingComposite.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#endif
#include "scene/Camera.hpp"
#include "scene/Level.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace lumin::render {
    namespace {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        constexpr float nrdDenoisingRange = 500000.0f;

        glm::vec3 normalizedLightDirection(glm::vec3 direction) {
            if (glm::dot(direction, direction) < 1.0e-6f) {
                direction = glm::vec3{-0.45f, -0.8f, -0.35f};
            }
            return glm::normalize(direction);
        }

        std::array<float, 16> matrixElements(const glm::mat4& matrix) {
            std::array<float, 16> result{};
            std::copy_n(glm::value_ptr(matrix), result.size(), result.begin());
            return result;
        }
#endif
    } // namespace

    void pipelines::DefaultRenderPipelineSession::commitAtmosphereFeature(
        const core::RenderFrameIdentity& identity) noexcept {
        if (!pendingAtmosphereSequence_) {
            return;
        }
        if (*pendingAtmosphereSequence_ != identity.sequence || atmosphereLutGpu_ == nullptr ||
            !atmosphereLutGpu_->hasPendingFrame() || !atmosphereLutScheduler_.hasActiveFrame()) {
            std::terminate();
        }
        atmosphereLutGpu_->commitSubmittedFrame();
        atmosphereLutScheduler_.commitSubmittedFrame(identity.sequence);
        pendingAtmosphereSequence_.reset();
        atmosphereForceRebuild_ = false;
    }

    void pipelines::DefaultRenderPipelineSession::commitHybridSurfaceFeature(
        const core::RenderFrameIdentity& identity) noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr || !hybridGi_->pendingSequence) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (*runtime.pendingSequence != identity.sequence || !runtime.pendingScenePlan || !runtime.pendingSceneUpdate ||
            runtime.directLighting == nullptr || !runtime.directLighting->hasPendingFrame() ||
            runtime.pendingSceneUpdate->frameSlot() != identity.frameSlot) {
            std::terminate();
        }
        runtime.scenePlanner->commit(*runtime.pendingScenePlan,
                                     gpu::GpuSceneCommitInfo{identity.frameSlot, true, true, true});
        runtime.sceneResources->finishUpdate(*runtime.pendingSceneUpdate, true);
        runtime.directLighting->commitSubmittedFrame();
        runtime.pendingSceneUpdate.reset();
        runtime.pendingScenePlan.reset();
#else
        static_cast<void>(identity);
#endif
    }

    void pipelines::DefaultRenderPipelineSession::commitGlobalIlluminationFeature(
        const core::RenderFrameIdentity& identity) noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr || !hybridGi_->pendingSequence) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (*runtime.pendingSequence != identity.sequence) {
            std::terminate();
        }
        if (runtime.sharc->hasPendingFrame()) {
            runtime.sharc->commitSubmittedFrame();
        }
        if (runtime.rayTracedGi->hasPendingFrame()) {
            runtime.rayTracedGi->commitSubmittedFrame();
        }
#else
        static_cast<void>(identity);
#endif
    }

    void pipelines::DefaultRenderPipelineSession::commitGiDenoiserFeature(
        const core::RenderFrameIdentity& identity) noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr || !hybridGi_->pendingSequence) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (*runtime.pendingSequence != identity.sequence) {
            std::terminate();
        }
        if (runtime.pendingNrdFrame) {
            if (runtime.pendingNrdFrame->sequence() != identity.sequence ||
                runtime.pendingNrdFrame->frameSlot() != identity.frameSlot) {
                std::terminate();
            }
            runtime.nrd->commitSubmittedFrame(*runtime.pendingNrdFrame);
            runtime.pendingNrdFrame.reset();
        }
        runtime.pendingSequence.reset();
#else
        static_cast<void>(identity);
#endif
    }

    void pipelines::DefaultRenderPipelineSession::discardAtmosphereFeature() noexcept {
        if (atmosphereLutGpu_ != nullptr) {
            atmosphereLutGpu_->discardPendingFrame();
        }
        if (pendingAtmosphereSequence_ && atmosphereLutScheduler_.hasActiveFrame()) {
            try {
                atmosphereLutScheduler_.abandonFrame(*pendingAtmosphereSequence_);
            } catch (...) {
            }
        }
        pendingAtmosphereSequence_.reset();
    }

    void pipelines::DefaultRenderPipelineSession::discardHybridSurfaceFeature() noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (runtime.directLighting != nullptr) {
            runtime.directLighting->discardPendingFrame();
        }
        if (runtime.pendingSceneUpdate) {
            try {
                runtime.sceneResources->finishUpdate(*runtime.pendingSceneUpdate, false);
            } catch (...) {
            }
        }
        runtime.pendingSceneUpdate.reset();
        runtime.pendingScenePlan.reset();
        runtime.pendingSequence.reset();
#endif
    }

    void pipelines::DefaultRenderPipelineSession::discardGlobalIlluminationFeature() noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (runtime.rayTracedGi != nullptr) {
            runtime.rayTracedGi->discardPendingFrame();
        }
        if (runtime.sharc != nullptr) {
            runtime.sharc->discardPendingFrame();
        }
#endif
    }

    void pipelines::DefaultRenderPipelineSession::discardGiDenoiserFeature() noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (runtime.pendingNrdFrame) {
            runtime.nrd->discardFrame(*runtime.pendingNrdFrame);
        } else if (runtime.nrd != nullptr) {
            runtime.nrd->discardPendingFrame();
        }
        runtime.pendingNrdFrame.reset();
#endif
    }

    void pipelines::DefaultRenderPipelineSession::addShadowFeaturePasses(core::RenderFeatureFrameContext& context) {
        const core::FrameSceneData& sceneData = context.blackboard().get<core::FrameSceneData>();
        const ShadowSettings& settings = sceneData.settings.get<ShadowSettings>(pipelines::feature_ids::shadow());
        core::ShadowData& shadows = context.blackboard().get<core::ShadowData>();
        const RasterPassTargets& targets = context.blackboard().get<RasterPassTargets>();
        if (!settings.enabled) {
            return;
        }
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        FrameGraph& graph = context.frameGraph();
        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            graph.addPass(
                "CSM clear " + std::to_string(cascade), FrameGraphPassType::Transfer,
                [shadow = shadows.cascades[cascade].graphResource](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::CopyDest);
                },
                [texture = shadows.cascades[cascade].texture](const FrameGraphContext& frameContext) {
                    frameContext.commandList->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, 1.0f,
                                                                       false, 0);
                });
            graph.addPass(
                "CSM cascade " + std::to_string(cascade), FrameGraphPassType::Graphics,
                [shadow = shadows.cascades[cascade].graphResource](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::DepthWrite);
                },
                [this, cascade, frameIndex, framebuffer = targets.shadowFramebuffers[cascade],
                 lightViewProjection = shadows.viewProjections[cascade]](const FrameGraphContext& frameContext) {
                    recordShadowPass(*frameContext.commandList, *framebuffer, frameIndex, cascade, lightViewProjection);
                });
        }
    }

    void pipelines::DefaultRenderPipelineSession::addGBufferFeaturePasses(core::RenderFeatureFrameContext& context) {
        const core::FrameSceneData& sceneData = context.blackboard().get<core::FrameSceneData>();
        core::RasterSurfaceData& surface = context.blackboard().get<core::RasterSurfaceData>();
        const RasterPassTargets& targets = context.blackboard().get<RasterPassTargets>();
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        FrameGraph& graph = context.frameGraph();
        const bool resetMotion = context.historyAction(core::HistoryDomain::Taa) == core::HistoryAction::FullReset;
        const glm::mat4 previousViewProjection =
            resetMotion ? sceneData.camera.viewProjection : previousViewProjection_;
        if (modelRenderer_ != nullptr) {
            modelRenderer_->sync(*sceneData.world, frameIndex, resetMotion);
        }
        graph.addPass(
            "G-buffer clear", FrameGraphPassType::Transfer,
            [&surface](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color :
                     {surface.position.graphResource, surface.normalRoughness.graphResource,
                      surface.albedoMetallic.graphResource, surface.motion.graphResource}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::CopyDest);
                }
                builder.writeTexture(surface.materialId.graphResource, nvrhi::ResourceStates::CopyDest);
                builder.writeTexture(surface.depth.graphResource, nvrhi::ResourceStates::CopyDest);
            },
            [&surface](const FrameGraphContext& frameContext) {
                const std::array<nvrhi::Color, 4> colors = {
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}, nvrhi::Color{0.0f, 0.0f, 1.0f, 1.0f},
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f}, nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}};
                const std::array<nvrhi::ITexture*, 4> images = {surface.position.texture,
                                                                surface.normalRoughness.texture,
                                                                surface.albedoMetallic.texture, surface.motion.texture};
                for (std::uint32_t index = 0; index < images.size(); ++index) {
                    frameContext.commandList->clearTextureFloat(images[index], nvrhi::AllSubresources, colors[index]);
                }
                frameContext.commandList->clearTextureUInt(surface.materialId.texture, nvrhi::AllSubresources,
                                                           gpu::GpuMaterialIndex::invalidValue);
                frameContext.commandList->clearDepthStencilTexture(surface.depth.texture, nvrhi::AllSubresources, true,
                                                                   1.0f, false, 0);
            });
        graph.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [&surface](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color :
                     {surface.position.graphResource, surface.normalRoughness.graphResource,
                      surface.albedoMetallic.graphResource, surface.motion.graphResource}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::RenderTarget);
                }
                builder.writeTexture(surface.materialId.graphResource, nvrhi::ResourceStates::RenderTarget);
                builder.writeTexture(surface.depth.graphResource, nvrhi::ResourceStates::DepthWrite);
            },
            [this, frameIndex, framebuffer = targets.surfaceFramebuffer,
             viewProjection = sceneData.camera.viewProjection,
             previousViewProjection](const FrameGraphContext& frameContext) {
                recordGBufferPass(*frameContext.commandList, *framebuffer, frameIndex, viewProjection,
                                  previousViewProjection);
            });
    }

    void
    pipelines::DefaultRenderPipelineSession::addHybridSurfaceFeaturePasses(core::RenderFeatureFrameContext& context) {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        const core::FrameSceneData& sceneData = context.blackboard().get<core::FrameSceneData>();
        core::RtSurfaceData& surfaceData = context.blackboard().get<core::RtSurfaceData>();
        HybridPassData& data = context.blackboard().get<HybridPassData>();
        const AtmospherePassData& atmosphereData = context.blackboard().get<AtmospherePassData>();
        FrameGraphResourceImporter* importer = context.blackboard().get<FrameImportServices>().importer;
        const DirectLightingFeatureSettings& lightingSettings =
            sceneData.settings.get<DirectLightingFeatureSettings>(pipelines::feature_ids::lightingComposite());
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        if (!data.active) {
            return;
        }
        if (importer == nullptr) {
            throw std::logic_error("Hybrid surface requires the frame resource importer.");
        }
        if (hybridGi_ == nullptr || hybridGi_->directLighting == nullptr || hybridGi_->scenePlanner == nullptr ||
            hybridGi_->sceneResources == nullptr) {
            throw std::logic_error("Hybrid surface requires the RT Scene and direct-lighting runtime.");
        }
        HybridGiState& runtime = *hybridGi_;
        if (runtime.pendingSequence || runtime.pendingScenePlan || runtime.pendingSceneUpdate ||
            runtime.pendingNrdFrame || runtime.sharc->hasPendingFrame() || runtime.rayTracedGi->hasPendingFrame() ||
            runtime.nrd->hasPendingFrame() || runtime.directLighting->hasPendingFrame()) {
            throw std::logic_error("Hybrid surface already owns an unfinished render frame.");
        }
        if (!atmosphereData.graphRecord || frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[frameIndex]) {
            throw std::logic_error("Hybrid surface requires the current atmosphere LUT graph and binding set.");
        }

        const bool resetMotion = context.historyAction(core::HistoryDomain::Taa) == core::HistoryAction::FullReset;
        const glm::mat4 previousViewProjection =
            resetMotion ? sceneData.camera.viewProjection : previousViewProjection_;
        if (modelRenderer_ != nullptr) {
            modelRenderer_->sync(*sceneData.world, frameIndex, resetMotion);
        }
        if (modelRenderer_ == nullptr || modelRenderer_->baseColorTextures().empty() ||
            modelRenderer_->baseColorTextures().size() != modelRenderer_->normalRoughnessTextures().size() ||
            !modelRenderer_->materialSampler()) {
            throw std::logic_error("Hybrid surface requires the shared model material table and texture descriptors.");
        }

        runtime.pendingSequence = context.identity().sequence;
        runtime.pendingScenePlan = runtime.scenePlanner->plan(world::SceneDelta{
            .changes = sceneData.changes,
            .snapshot = sceneData.world,
        });
        runtime.pendingSceneUpdate = runtime.sceneResources->recordUpdate(
            context.frameGraph(), *runtime.pendingScenePlan, context.identity().frameSlot, true);

        const gpu::GpuScenePreparedUpdate& update = *runtime.pendingSceneUpdate;
        data.sceneDescriptors = runtime.sceneResources->candidateDescriptors(update);
        data.geometry = runtime.sceneResources->candidateGeometry(update);
        const std::span<const gpu::GpuGeometryFrameGraphResources> geometryResources = update.geometryResources();
        if (geometryResources.size() != data.geometry.size()) {
            throw std::logic_error("GPU Scene native and FrameGraph geometry arrays do not match.");
        }
        data.vertices.clear();
        data.indices.clear();
        data.vertices.reserve(geometryResources.size());
        data.indices.reserve(geometryResources.size());
        for (const gpu::GpuGeometryFrameGraphResources& geometry : geometryResources) {
            data.vertices.push_back(geometry.vertices);
            data.indices.push_back(geometry.indices);
        }
        data.tlas = update.tlasResource();
        data.instances = update.instanceRecordsResource();
        const nvrhi::BufferHandle& materialBuffer = modelRenderer_->materialBuffer(frameIndex);
        data.materials = importer->importBuffer(
            "rt.materials", FrameGraphBufferDesc{.size = materialBuffer->getDesc().byteSize,
                                                 .buffer = materialBuffer,
                                                 .initialState = modelRenderer_->materialBufferInitialState(frameIndex),
                                                 .finalState = nvrhi::ResourceStates::ShaderResource});
        data.sceneDescriptors.materials = materialBuffer;
        data.baseColorTextures.clear();
        data.normalRoughnessTextures.clear();
        const std::span<const nvrhi::TextureHandle> baseColorTextures = modelRenderer_->baseColorTextures();
        const std::span<const nvrhi::TextureHandle> normalRoughnessTextures = modelRenderer_->normalRoughnessTextures();
        data.baseColorTextures.reserve(baseColorTextures.size());
        data.normalRoughnessTextures.reserve(normalRoughnessTextures.size());
        for (std::size_t index = 0; index < baseColorTextures.size(); ++index) {
            const auto importMaterialTexture = [&](const char* prefix, const nvrhi::TextureHandle& texture) {
                return importer->importTexture(
                    std::string{prefix} + std::to_string(index),
                    FrameGraphTextureDesc{.texture = texture,
                                          .initialState = nvrhi::ResourceStates::ShaderResource,
                                          .finalState = nvrhi::ResourceStates::ShaderResource});
            };
            data.baseColorTextures.push_back(
                importMaterialTexture("rt.material-base-color.", baseColorTextures[index]));
            data.normalRoughnessTextures.push_back(
                importMaterialTexture("rt.material-normal-roughness.", normalRoughnessTextures[index]));
        }
        data.sceneReadyPass = update.accelerationStructurePass();
        if (!data.sceneReadyPass.isValid()) {
            data.sceneReadyPass = update.uploadPass();
        }

        const scene::DirectionalLight& sun = sceneData.world->environment().sun;
        const glm::vec3 toSun = -normalizedLightDirection(sun.direction);
        const glm::vec4 sunRadiance = gi::makeRayTracingSunRadiance(sun, lightingSettings.enabled);
        const gi::RayTracingEnvironmentBindings environment{
            .atmosphere = atmosphereConsumerBindingSets_[frameIndex],
        };
        const gi::RayTracingEnvironmentGraphResources environmentResources{
            .atmosphere = atmosphereData.graphRecord->resources,
        };
        const gi::RayTracedDiConstants directConstants{
            .inverseViewProjection = glm::inverse(sceneData.camera.viewProjection),
            .previousViewProjection = previousViewProjection,
            .cameraPosition = sceneData.camera.position,
            .cameraForward = sceneData.camera.forward,
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderSize = glm::vec4{static_cast<float>(context.identity().extent.width),
                                    static_cast<float>(context.identity().extent.height),
                                    -sceneData.camera.jitter.x / static_cast<float>(context.identity().extent.width),
                                    sceneData.camera.jitter.y / static_cast<float>(context.identity().extent.height)},
            .traceParameters = glm::vec4{0.001f, sceneData.camera.farPlane, lightingSettings.enabled ? 1.0f : 0.0f,
                                         static_cast<float>(context.identity().sequence.value())},
        };
        data.surfacePass = runtime.directLighting->record(
            context.frameGraph(), frameIndex, true, directConstants, data.surface,
            gi::RayTracedGiSceneBindings{
                .descriptors = data.sceneDescriptors,
                .geometry = data.geometry,
                .baseColorTextures = baseColorTextures,
                .normalRoughnessTextures = normalRoughnessTextures,
                .materialSampler = modelRenderer_->materialSampler(),
            },
            gi::RayTracedGiSceneGraphResources{
                .tlas = data.tlas,
                .instances = data.instances,
                .materials = data.materials,
                .vertices = std::span<const FrameGraphResourceHandle>{data.vertices},
                .indices = std::span<const FrameGraphResourceHandle>{data.indices},
                .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.baseColorTextures},
                .normalRoughnessTextures = std::span<const FrameGraphResourceHandle>{data.normalRoughnessTextures},
                .readyPass = data.sceneReadyPass,
            },
            environment, environmentResources);

        core::GpuSceneData gpuScene;
        gpuScene.topLevelAccelerationStructure = core::AccelerationStructureFrameData{
            .accelerationStructure = data.sceneDescriptors.tlas,
            .graphResource = data.tlas,
            .readyPass = data.sceneReadyPass,
        };
        gpuScene.instances = core::BufferFrameData{.buffer = data.sceneDescriptors.instances,
                                                   .graphResource = data.instances,
                                                   .readyPass = data.sceneReadyPass};
        gpuScene.materials = core::BufferFrameData{
            .buffer = materialBuffer, .graphResource = data.materials, .readyPass = data.sceneReadyPass};
        gpuScene.vertices.reserve(data.geometry.size());
        gpuScene.indices.reserve(data.geometry.size());
        for (std::size_t index = 0; index < data.geometry.size(); ++index) {
            gpuScene.vertices.push_back(core::BufferFrameData{.buffer = data.geometry[index].vertices,
                                                              .graphResource = data.vertices[index],
                                                              .readyPass = data.sceneReadyPass});
            gpuScene.indices.push_back(core::BufferFrameData{.buffer = data.geometry[index].indices,
                                                             .graphResource = data.indices[index],
                                                             .readyPass = data.sceneReadyPass});
        }
        surfaceData.worldPositionHitDistance.readyPass = data.surfacePass;
        surfaceData.normalRoughness.readyPass = data.surfacePass;
        surfaceData.albedoMetallic.readyPass = data.surfacePass;
        surfaceData.motion.readyPass = data.surfacePass;
        surfaceData.materialId.readyPass = data.surfacePass;
        surfaceData.viewDepth.readyPass = data.surfacePass;
        surfaceData.visibility.readyPass = data.surfacePass;
        surfaceData.directRadiance.readyPass = data.surfacePass;
        context.blackboard().set(std::move(gpuScene));
#else
        static_cast<void>(context);
#endif
    }

    void
    pipelines::DefaultRenderPipelineSession::addAtmosphereLutFeaturePasses(core::RenderFeatureFrameContext& context) {
        const core::FrameSceneData& sceneData = context.blackboard().get<core::FrameSceneData>();
        AtmospherePassData& passData = context.blackboard().get<AtmospherePassData>();
        const AtmosphereRenderSettings& settings =
            sceneData.settings.get<AtmosphereRenderSettings>(pipelines::feature_ids::atmosphere());
        if (atmosphereLutGpu_ == nullptr) {
            throw std::logic_error("Atmosphere LUT GPU resources are not initialized.");
        }
        if (pendingAtmosphereSequence_) {
            throw std::logic_error("Default pipeline session already owns a pending atmosphere LUT frame.");
        }

        scene::SceneEnvironment environment = sceneData.world->environment();
        environment.atmosphere.enabled = environment.atmosphere.enabled && settings.enabled;
        const core::CameraFrameData& camera = sceneData.camera;
        const atmosphere::AtmosphereViewInput view{
            .cameraPositionWorld = glm::vec3{camera.position},
            .view = camera.view,
            .projection = camera.projection,
            .nearPlaneWorld = camera.nearPlane,
            .farPlaneWorld = camera.farPlane,
            .renderExtent = context.identity().extent,
        };
        const atmosphere::AtmosphereLutSignatures signatures =
            atmosphere::makeAtmosphereLutSignatures(environment, view);
        const atmosphere::AtmosphereLutPlan lutPlan = atmosphereLutScheduler_.beginFrame(
            atmosphere::AtmosphereLutFrameInput{context.identity().sequence, signatures, atmosphereForceRebuild_});
        pendingAtmosphereSequence_ = context.identity().sequence;
        const atmosphere::AtmosphereGpuConstants constants = atmosphere::buildAtmosphereGpuConstants(environment, view);
        passData.graphRecord =
            atmosphereLutGpu_->record(context.frameGraph(), context.identity().frameSlot.value(), true, constants,
                                      atmosphere::makeAtmosphereLutPassPlan(lutPlan));

        const atmosphere::AtmosphereLutNativeResources& native = atmosphereLutGpu_->nativeResources();
        const auto publishTexture = [&native, &passData](atmosphere::AtmosphereLut lut) {
            const nvrhi::TextureHandle& texture = native.texture(lut);
            const nvrhi::TextureDesc& desc = texture->getDesc();
            return core::TextureFrameData{
                .texture = texture,
                .graphResource = passData.graphRecord->resources.texture(lut),
                .readyPass = passData.graphRecord->passes.pass(lut),
                .format = desc.format,
                .extent = {desc.width, desc.height},
            };
        };
        context.blackboard().set(core::AtmosphereData{
            .transmittance = publishTexture(atmosphere::AtmosphereLut::Transmittance),
            .multiScattering = publishTexture(atmosphere::AtmosphereLut::MultiScattering),
            .skyView = publishTexture(atmosphere::AtmosphereLut::SkyView),
            .aerialPerspective = publishTexture(atmosphere::AtmosphereLut::AerialPerspective),
        });
    }

    void pipelines::DefaultRenderPipelineSession::addGlobalIlluminationFeaturePasses(
        core::RenderFeatureFrameContext& context) {
        const core::FrameSceneData& sceneData = context.blackboard().get<core::FrameSceneData>();
        core::IndirectLightingData& indirect = context.blackboard().get<core::IndirectLightingData>();
        const GlobalIlluminationSettings& settings =
            sceneData.settings.get<GlobalIlluminationSettings>(pipelines::feature_ids::globalIllumination());
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        core::HistoryAction fallbackAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
        fallbackAction =
            core::strongerHistoryAction(fallbackAction, context.historyAction(core::HistoryDomain::NrdSpecular));
        fallbackAction = core::strongerHistoryAction(fallbackAction, context.historyAction(core::HistoryDomain::Sharc));

        if (!shouldUseHybridGi(settings, *sceneData.world)) {
            const core::RasterSurfaceData& surface = context.blackboard().get<core::RasterSurfaceData>();
            if (fallbackAction != core::HistoryAction::Keep) {
                globalIllumination_->invalidateHistory();
            }
            globalIllumination_->addPasses(context.frameGraph(),
                                           gi::FrameInfo{
                                               *sceneData.world,
                                               frameIndex,
                                               context.identity().sequence.value(),
                                               settings.ssaoEnabled,
                                               fallbackAction == core::HistoryAction::FullReset,
                                               {context.identity().extent.width, context.identity().extent.height},
                                               surface.position.graphResource,
                                               surface.normalRoughness.graphResource,
                                               surface.albedoMetallic.graphResource,
                                               surface.motion.graphResource,
                                               surface.depth.graphResource,
                                               indirect.combined.graphResource,
                                           });
            return;
        }

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        HybridPassData& data = context.blackboard().get<HybridPassData>();
        const DirectLightingFeatureSettings& lightingSettings =
            sceneData.settings.get<DirectLightingFeatureSettings>(pipelines::feature_ids::lightingComposite());
        HybridGiState& runtime = *hybridGi_;
        if (!runtime.pendingSequence || *runtime.pendingSequence != context.identity().sequence ||
            !runtime.pendingScenePlan || !runtime.pendingSceneUpdate || !runtime.directLighting->hasPendingFrame() ||
            runtime.pendingNrdFrame || runtime.sharc->hasPendingFrame() || runtime.rayTracedGi->hasPendingFrame() ||
            runtime.nrd->hasPendingFrame()) {
            throw std::logic_error("Hybrid GI requires the current frame's pending surface transaction.");
        }
        const AtmospherePassData& atmosphereData = context.blackboard().get<AtmospherePassData>();
        const core::RtSurfaceData& surface = context.blackboard().get<core::RtSurfaceData>();
        if (!atmosphereData.graphRecord || frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[frameIndex]) {
            throw std::logic_error("Hybrid GI requires the current atmosphere LUT graph and binding set.");
        }

        const scene::DirectionalLight& sun = sceneData.world->environment().sun;
        const glm::vec3 toSun = -normalizedLightDirection(sun.direction);
        const glm::vec4 sunRadiance = gi::makeRayTracingSunRadiance(sun, lightingSettings.enabled);
        gi::SharcInvalidationInputs sharcInvalidation{
            .cameraCut = context.changes().containsAny(core::HistoryReason::CameraCut),
            .topologyChanged = world::hasAnyChange(sceneData.changes, world::SceneChangeMask::InstanceTopology),
            .geometryChanged = world::hasAnyChange(sceneData.changes, world::SceneChangeMask::Geometry),
            .materialChanged = world::hasAnyChange(sceneData.changes, world::SceneChangeMask::TransformOrMaterial |
                                                                          world::SceneChangeMask::MaterialBinding),
            .lightingChanged = world::hasAnyChange(sceneData.changes, world::SceneChangeMask::Lighting),
            .atmosphereChanged = world::hasAnyChange(sceneData.changes, world::SceneChangeMask::Atmosphere),
        };
        const core::HistoryAction sharcAction = context.historyAction(core::HistoryDomain::Sharc);
        if (sharcAction == core::HistoryAction::FullReset) {
            sharcInvalidation.topologyChanged = true;
        } else if (sharcAction == core::HistoryAction::SoftReset) {
            sharcInvalidation.materialChanged = true;
        }

        const gi::RayTracingEnvironmentBindings environment{
            .atmosphere = atmosphereConsumerBindingSets_[frameIndex],
        };
        const gi::RayTracingEnvironmentGraphResources environmentResources{
            .atmosphere = atmosphereData.graphRecord->resources,
        };
        const gi::SharcUpdateSceneBindings sceneBindings{
            .descriptors = data.sceneDescriptors,
            .geometry = data.geometry,
            .baseColorTextures = modelRenderer_->baseColorTextures(),
            .normalRoughnessTextures = modelRenderer_->normalRoughnessTextures(),
            .materialSampler = modelRenderer_->materialSampler(),
        };
        const gi::SharcUpdateSceneGraphResources sceneGraphResources{
            .tlas = data.tlas,
            .instances = data.instances,
            .materials = data.materials,
            .vertices = std::span<const FrameGraphResourceHandle>{data.vertices},
            .indices = std::span<const FrameGraphResourceHandle>{data.indices},
            .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.baseColorTextures},
            .normalRoughnessTextures = std::span<const FrameGraphResourceHandle>{data.normalRoughnessTextures},
            .readyPass = data.sceneReadyPass,
        };
        const gi::SharcFrameParameters sharcFrame{
            .cameraPosition = sceneData.camera.position,
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderWidth = context.identity().extent.width,
            .renderHeight = context.identity().extent.height,
            .minTraceDistance = 0.001f,
            .maxTraceDistance = sceneData.camera.farPlane,
        };
        if (runtime.sharcEnabled) {
            data.sharcRecord =
                runtime.sharc->record(context.frameGraph(), frameIndex, true, sharcFrame, sharcInvalidation,
                                      environment, environmentResources,
                                      gi::SharcUpdateFrameGraphInputs{surface.worldPositionHitDistance.graphResource,
                                                                      surface.normalRoughness.graphResource,
                                                                      surface.albedoMetallic.graphResource},
                                      sceneBindings, sceneGraphResources);
        }

        const core::HistoryAction diffuseAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
        const core::HistoryAction specularAction = context.historyAction(core::HistoryDomain::NrdSpecular);
        const bool resetNrdMotion = !hasSubmittedFrame_ || diffuseAction == core::HistoryAction::FullReset ||
                                    specularAction == core::HistoryAction::FullReset;
        const glm::vec2 previousJitter = resetNrdMotion ? sceneData.camera.jitter : previousJitter_;
        const glm::vec2 currentEffectiveJitter{
            -sceneData.camera.jitter.x / static_cast<float>(context.identity().extent.width),
            sceneData.camera.jitter.y / static_cast<float>(context.identity().extent.height)};
        const glm::vec2 previousEffectiveJitter{-previousJitter.x / static_cast<float>(context.identity().extent.width),
                                                previousJitter.y /
                                                    static_cast<float>(context.identity().extent.height)};
        const glm::vec2 effectiveJitterDelta = currentEffectiveJitter - previousEffectiveJitter;
        const gi::RayTracedGiConstants rayTracingConstants{
            .cameraPosition = sceneData.camera.position,
            .cameraForward = sceneData.camera.forward,
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderSize = glm::vec4{static_cast<float>(context.identity().extent.width),
                                    static_cast<float>(context.identity().extent.height), effectiveJitterDelta.x,
                                    effectiveJitterDelta.y},
            .traceParameters = glm::vec4{0.001f, sceneData.camera.farPlane,
                                         static_cast<float>(context.identity().sequence.value()), nrdDenoisingRange},
        };
        data.rayTracedSignals = runtime.rayTracedGi->record(
            context.frameGraph(), frameIndex, true, rayTracingConstants,
            gi::RayTracedGiFrameGraphInputs{surface.worldPositionHitDistance.graphResource,
                                            surface.normalRoughness.graphResource, surface.albedoMetallic.graphResource,
                                            surface.motion.graphResource},
            gi::RayTracedGiSceneBindings{
                .descriptors = data.sceneDescriptors,
                .geometry = data.geometry,
                .baseColorTextures = modelRenderer_->baseColorTextures(),
                .normalRoughnessTextures = modelRenderer_->normalRoughnessTextures(),
                .materialSampler = modelRenderer_->materialSampler(),
            },
            gi::RayTracedGiSceneGraphResources{
                .tlas = data.tlas,
                .instances = data.instances,
                .materials = data.materials,
                .vertices = std::span<const FrameGraphResourceHandle>{data.vertices},
                .indices = std::span<const FrameGraphResourceHandle>{data.indices},
                .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.baseColorTextures},
                .normalRoughnessTextures = std::span<const FrameGraphResourceHandle>{data.normalRoughnessTextures},
                .readyPass = data.sceneReadyPass,
            },
            environment, environmentResources, data.sharcRecord ? &*data.sharcRecord : nullptr);
        if (data.sharcRecord) {
            static_cast<void>(runtime.sharc->recordStatisticsReadback(context.frameGraph(), *data.sharcRecord,
                                                                      data.rayTracedSignals->tracePass));
        }
        const gi::RayTracedGiSignalResources& signals = runtime.rayTracedGi->signals(frameIndex);
        indirect.diffuse = core::TextureFrameData{.texture = signals.diffuseRadianceHitDistance,
                                                  .graphResource = data.rayTracedSignals->diffuseRadianceHitDistance,
                                                  .readyPass = data.rayTracedSignals->tracePass,
                                                  .format = signals.diffuseRadianceHitDistance->getDesc().format,
                                                  .extent = context.identity().extent};
        indirect.specular = core::TextureFrameData{.texture = signals.specularRadianceHitDistance,
                                                   .graphResource = data.rayTracedSignals->specularRadianceHitDistance,
                                                   .readyPass = data.rayTracedSignals->tracePass,
                                                   .format = signals.specularRadianceHitDistance->getDesc().format,
                                                   .extent = context.identity().extent};
        data.globalIlluminationActive = true;
#endif
    }

    void pipelines::DefaultRenderPipelineSession::addGiDenoiserFeaturePasses(core::RenderFeatureFrameContext& context) {
        HybridPassData& data = context.blackboard().get<HybridPassData>();
        if (!data.active) {
            // Raster recipe 已把 legacy GI 写入 DenoisedLightingData::combined，不需要任何 RT/NRD 输入。
            return;
        }
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        const core::FrameSceneData& sceneData = context.blackboard().get<core::FrameSceneData>();
        const core::RtSurfaceData& surfaceData = context.blackboard().get<core::RtSurfaceData>();
        const core::TemporalOutputData& temporal = context.blackboard().get<core::TemporalOutputData>();
        core::SceneHdrData& sceneHdr = context.blackboard().get<core::SceneHdrData>();
        core::DenoisedLightingData& denoised = context.blackboard().get<core::DenoisedLightingData>();
        const GlobalIlluminationSettings& settings =
            sceneData.settings.get<GlobalIlluminationSettings>(pipelines::feature_ids::globalIllumination());
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        if (!data.globalIlluminationActive) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (!runtime.pendingSequence || *runtime.pendingSequence != context.identity().sequence ||
            !runtime.pendingSceneUpdate || !data.rayTracedSignals || runtime.pendingNrdFrame) {
            throw std::logic_error("NRD requires the current frame's RT GI transaction.");
        }

        const gi::RayTracedGiSignalResources& signals = runtime.rayTracedGi->signals(frameIndex);
        const gi::RayTracedGiGraphSignals& graphSignals = *data.rayTracedSignals;
        nvrhi::TextureHandle diffuseInput = signals.diffuseRadianceHitDistance;
        nvrhi::TextureHandle specularInput = signals.specularRadianceHitDistance;
        FrameGraphResourceHandle diffuseGraphInput = graphSignals.diffuseRadianceHitDistance;
        FrameGraphResourceHandle specularGraphInput = graphSignals.specularRadianceHitDistance;
        FrameGraphPassHandle signalReadyPass = graphSignals.tracePass;

        if (settings.nrdEnabled) {
            const core::HistoryAction diffuseAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
            const core::HistoryAction specularAction = context.historyAction(core::HistoryDomain::NrdSpecular);
            const bool resetCameraHistory = !hasSubmittedFrame_ || diffuseAction == core::HistoryAction::FullReset ||
                                            specularAction == core::HistoryAction::FullReset;
            const glm::mat4& previousView = resetCameraHistory ? sceneData.camera.view : previousView_;
            const glm::mat4& previousProjection =
                resetCameraHistory ? sceneData.camera.projection : previousProjection_;
            const glm::vec2 previousJitter = resetCameraHistory ? sceneData.camera.jitter : previousJitter_;

            gi::NrdCameraData cameraData;
            cameraData.viewToClip = matrixElements(sceneData.camera.projection);
            cameraData.viewToClipPrevious = matrixElements(previousProjection);
            cameraData.worldToView = matrixElements(sceneData.camera.view);
            cameraData.worldToViewPrevious = matrixElements(previousView);
            cameraData.jitter = {sceneData.camera.jitter.x, sceneData.camera.jitter.y};
            cameraData.jitterPrevious = {previousJitter.x, previousJitter.y};

            runtime.pendingNrdFrame = runtime.nrd->record(
                context.frameGraph(),
                gi::NrdFrameParameters{
                    .frameSlot = context.identity().frameSlot,
                    .sequence = context.identity().sequence,
                    .extent = context.identity().extent,
                    .camera = cameraData,
                    .diffuseHistory = diffuseAction,
                    .specularHistory = specularAction,
                    .cameraCut = context.changes().containsAny(core::HistoryReason::CameraCut),
                    .renderResourcesRecreated = context.changes().containsAny(core::HistoryReason::RenderExtentChanged |
                                                                              core::HistoryReason::SwapchainRecreated |
                                                                              core::HistoryReason::DeviceRecovered),
                    .frameSlotFenceWaited = true,
                    .timeDeltaMilliseconds = 0.0f,
                    .denoisingRange = nrdDenoisingRange,
                },
                gi::NrdSignalBindings{
                    .diffuseRadianceHitDistance = gi::NrdTextureBinding{signals.diffuseRadianceHitDistance.Get(),
                                                                        graphSignals.diffuseRadianceHitDistance},
                    .specularRadianceHitDistance = gi::NrdTextureBinding{signals.specularRadianceHitDistance.Get(),
                                                                         graphSignals.specularRadianceHitDistance},
                    .viewZ = gi::NrdTextureBinding{signals.viewZ.Get(), graphSignals.viewZ},
                    .normalRoughness =
                        gi::NrdTextureBinding{signals.normalRoughness.Get(), graphSignals.normalRoughness},
                    .motion = gi::NrdTextureBinding{signals.motion.Get(), graphSignals.motion},
                });

            const std::span<const FrameGraphPassHandle> nrdPasses = runtime.pendingNrdFrame->passes();
            if (nrdPasses.empty()) {
                throw std::logic_error("NRD did not register any denoising dispatches.");
            }
            const gi::NrdOutputResources& nrdOutputs = runtime.nrd->outputs();
            const gi::NrdGraphOutputs& nrdGraphOutputs = runtime.pendingNrdFrame->outputs();
            diffuseInput = nrdOutputs.diffuseRadianceHitDistance;
            specularInput = nrdOutputs.specularRadianceHitDistance;
            diffuseGraphInput = nrdGraphOutputs.diffuseRadianceHitDistance;
            specularGraphInput = nrdGraphOutputs.specularRadianceHitDistance;
            signalReadyPass = nrdPasses.back();
        }
        denoised.diffuse = core::TextureFrameData{.texture = diffuseInput,
                                                  .graphResource = diffuseGraphInput,
                                                  .readyPass = signalReadyPass,
                                                  .format = diffuseInput->getDesc().format,
                                                  .extent = context.identity().extent};
        denoised.specular = core::TextureFrameData{.texture = specularInput,
                                                   .graphResource = specularGraphInput,
                                                   .readyPass = signalReadyPass,
                                                   .format = specularInput->getDesc().format,
                                                   .extent = context.identity().extent};
        const FrameGraphPassHandle compositePass = runtime.composite->record(
            context.frameGraph(),
            gi::GiCompositeFrameParameters{
                .frameSlot = context.identity().frameSlot,
                .extent = context.identity().extent,
                .cameraPosition = glm::vec3{sceneData.camera.position},
                .frameSlotFenceWaited = true,
            },
            gi::GiCompositeResources{
                .diffuseRadianceHitDistance = diffuseInput,
                .specularRadianceHitDistance = specularInput,
                .position = surfaceData.worldPositionHitDistance.texture,
                .normalRoughness = surfaceData.normalRoughness.texture,
                .albedoMetallic = surfaceData.albedoMetallic.texture,
                .materialId = surfaceData.materialId.texture,
                .materials = data.sceneDescriptors.materials,
                // Hybrid 暂时复用 TAA 物理目标暂存间接光，后续 lighting composite 完成后才允许 TAA 覆写。
                .globalIllumination = temporal.color.texture,
            },
            gi::GiCompositeGraphResources{
                .diffuseRadianceHitDistance = diffuseGraphInput,
                .specularRadianceHitDistance = specularGraphInput,
                .position = surfaceData.worldPositionHitDistance.graphResource,
                .normalRoughness = surfaceData.normalRoughness.graphResource,
                .albedoMetallic = surfaceData.albedoMetallic.graphResource,
                .materialId = surfaceData.materialId.graphResource,
                .materials = data.materials,
                .globalIllumination = temporal.color.graphResource,
            },
            signalReadyPass);
        denoised.combined = temporal.color;
        denoised.combined.readyPass = compositePass;
        const gi::RayTracedDiFrameResources& surface = runtime.directLighting->signals(frameIndex);
        const FrameGraphPassHandle lightingPass =
            runtime.lightingComposite->record(context.frameGraph(),
                                              gi::HybridLightingCompositeFrameParameters{
                                                  .frameSlot = context.identity().frameSlot,
                                                  .extent = context.identity().extent,
                                                  .mode = gi::HybridLightingCompositeMode::DirectAndIndirect,
                                                  .frameSlotFenceWaited = true,
                                              },
                                              gi::HybridLightingCompositeResources{
                                                  .directRadiance = surface.directRadiance,
                                                  .indirectRadiance = temporal.color.texture,
                                                  .output = sceneHdr.color.texture,
                                              },
                                              gi::HybridLightingCompositeGraphResources{
                                                  .directRadiance = data.surface.directRadiance,
                                                  .indirectRadiance = temporal.color.graphResource,
                                                  .output = sceneHdr.color.graphResource,
                                              },
                                              compositePass);
        sceneHdr.color.readyPass = lightingPass;
#else
        static_cast<void>(context);
#endif
    }

    void
    pipelines::DefaultRenderPipelineSession::addSkyCompositeFeaturePasses(core::RenderFeatureFrameContext& context) {
        core::SceneHdrData& sceneHdr = context.blackboard().get<core::SceneHdrData>();
        const core::DenoisedLightingData& indirect = context.blackboard().get<core::DenoisedLightingData>();
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        HybridPassData& hybridData = context.blackboard().get<HybridPassData>();
        // RT primary miss 已经把 atmosphere environment 写入 directRadiance；Hybrid 的 compute composite
        // 只需等待 GI denoiser，不再声明旧的 raster sky framebuffer/CSM 输入。
        if (hybridData.active) {
            if (hybridData.globalIlluminationActive) {
                return;
            }
            if (hybridGi_ == nullptr || hybridGi_->lightingComposite == nullptr ||
                hybridGi_->directLighting == nullptr || !hybridData.surface.directRadiance.isValid() ||
                !hybridData.surfacePass.isValid()) {
                throw std::logic_error("Hybrid direct-only composite requires the RTDI surface pass.");
            }

            const gi::RayTracedDiFrameResources& surface = hybridGi_->directLighting->signals(frameIndex);
            static_cast<void>(
                hybridGi_->lightingComposite->record(context.frameGraph(),
                                                     gi::HybridLightingCompositeFrameParameters{
                                                         .frameSlot = context.identity().frameSlot,
                                                         .extent = context.identity().extent,
                                                         .mode = gi::HybridLightingCompositeMode::DirectOnly,
                                                         .frameSlotFenceWaited = true,
                                                     },
                                                     gi::HybridLightingCompositeResources{
                                                         .directRadiance = surface.directRadiance,
                                                         .indirectRadiance = indirect.combined.texture,
                                                         .output = sceneHdr.color.texture,
                                                     },
                                                     gi::HybridLightingCompositeGraphResources{
                                                         .directRadiance = hybridData.surface.directRadiance,
                                                         .indirectRadiance = indirect.combined.graphResource,
                                                         .output = sceneHdr.color.graphResource,
                                                     },
                                                     hybridData.surfacePass));
            return;
        }
#endif
        const AtmospherePassData& atmosphereData = context.blackboard().get<AtmospherePassData>();
        const PostProcessPassData& postProcess = context.blackboard().get<PostProcessPassData>();
        FrameGraph& graph = context.frameGraph();
        if (!atmosphereData.graphRecord || frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[frameIndex]) {
            throw std::logic_error("Sky composite requires the atmosphere LUT record from the current frame.");
        }
        graph.addPass(
            "Procedural sky clear", FrameGraphPassType::Transfer,
            [&sceneHdr](FrameGraphBuilder& builder) {
                builder.writeTexture(sceneHdr.color.graphResource, nvrhi::ResourceStates::CopyDest);
            },
            [texture = sceneHdr.color.texture](const FrameGraphContext& frameContext) {
                frameContext.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                            nvrhi::Color{0.035f, 0.04f, 0.05f, 1.0f});
            });
        graph.addPass(
            "Procedural sky", FrameGraphPassType::Graphics,
            [&sceneHdr, &indirect, &atmosphereData](FrameGraphBuilder& builder) {
                builder.readTexture(indirect.combined.graphResource, nvrhi::ResourceStates::ShaderResource);
                for (const FrameGraphResourceHandle lut : atmosphereData.graphRecord->resources.textures) {
                    builder.readTexture(lut, nvrhi::ResourceStates::ShaderResource);
                }
                builder.read(atmosphereData.graphRecord->resources.constants, nvrhi::ResourceStates::ConstantBuffer);
                builder.writeTexture(sceneHdr.color.graphResource, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex, framebuffer = postProcess.lightingFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, skyPipeline_, frameIndex,
                                     atmosphereConsumerBindingSets_[frameIndex]);
            });
    }

    void
    pipelines::DefaultRenderPipelineSession::addDirectLightingFeaturePasses(core::RenderFeatureFrameContext& context) {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        const HybridPassData& hybridData = context.blackboard().get<HybridPassData>();
        if (hybridData.active) {
            return;
        }
#endif
        if (modelRenderer_ == nullptr) {
            return;
        }
        const core::RasterSurfaceData& surface = context.blackboard().get<core::RasterSurfaceData>();
        const core::ShadowData& shadows = context.blackboard().get<core::ShadowData>();
        const core::DenoisedLightingData& indirect = context.blackboard().get<core::DenoisedLightingData>();
        core::SceneHdrData& sceneHdr = context.blackboard().get<core::SceneHdrData>();
        const PostProcessPassData& postProcess = context.blackboard().get<PostProcessPassData>();
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "Deferred lighting", FrameGraphPassType::Graphics,
            [&surface, &shadows, &indirect, &sceneHdr](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input :
                     {surface.position.graphResource, surface.normalRoughness.graphResource,
                      surface.albedoMetallic.graphResource, indirect.combined.graphResource,
                      surface.materialId.graphResource}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                builder.read(surface.materials.graphResource, nvrhi::ResourceStates::ShaderResource);
                for (const core::TextureFrameData& shadow : shadows.cascades) {
                    builder.readTexture(shadow.graphResource, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(sceneHdr.color.graphResource, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex, framebuffer = postProcess.lightingFramebuffer](const FrameGraphContext& frameContext) {
                recordDirectLightingPass(*frameContext.commandList, *framebuffer, frameIndex);
            });
    }

    void pipelines::DefaultRenderPipelineSession::addTemporalAaFeaturePasses(core::RenderFeatureFrameContext& context) {
        const core::SceneHdrData& sceneHdr = context.blackboard().get<core::SceneHdrData>();
        core::TemporalOutputData& temporal = context.blackboard().get<core::TemporalOutputData>();
        PostProcessPassData& postProcess = context.blackboard().get<PostProcessPassData>();
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        FrameGraph& graph = context.frameGraph();
        const core::HistoryAction action = context.historyAction(core::HistoryDomain::Taa);
        postProcess.uniforms.renderOptions.x =
            action == core::HistoryAction::Keep && postFxResources_.historyValid(postProcess.historyReadSlot) ? 1.0f
                                                                                                              : 0.0f;
        if (action != core::HistoryAction::Keep) {
            // 历史失效时统一两帧 jitter 基线，避免坐标约定切换或 camera cut 携带旧抖动差。
            postProcess.uniforms.temporalOptions.z = postProcess.uniforms.temporalOptions.x;
            postProcess.uniforms.temporalOptions.w = postProcess.uniforms.temporalOptions.y;
        }
        postFxResources_.updateUniforms(frameIndex, postProcess.uniforms);

        graph.addPass(
            "TAA clear", FrameGraphPassType::Transfer,
            [&temporal](FrameGraphBuilder& builder) {
                builder.writeTexture(temporal.color.graphResource, nvrhi::ResourceStates::CopyDest);
            },
            [texture = temporal.color.texture](const FrameGraphContext& frameContext) {
                frameContext.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                            nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f});
            });
        graph.addPass(
            "TAA resolve", FrameGraphPassType::Graphics,
            [&sceneHdr, &temporal](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input :
                     {sceneHdr.color.graphResource, sceneHdr.position.graphResource, sceneHdr.motion.graphResource,
                      temporal.historyRead.graphResource}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(temporal.color.graphResource, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex, framebuffer = postProcess.temporalFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, temporalAaPipeline_, frameIndex);
            });
        graph.addPass(
            "TAA history copy", FrameGraphPassType::Transfer,
            [&temporal](FrameGraphBuilder& builder) {
                builder.readTexture(temporal.color.graphResource, nvrhi::ResourceStates::CopySource);
                builder.writeTexture(temporal.historyWrite.graphResource, nvrhi::ResourceStates::CopyDest);
            },
            [this, frameIndex](const FrameGraphContext& frameContext) {
                recordHistoryCopy(*frameContext.commandList, frameIndex);
            });
        graph.addPass(
            "TAA history ready", FrameGraphPassType::Transfer,
            [&temporal](FrameGraphBuilder& builder) {
                builder.readTexture(temporal.historyWrite.graphResource, nvrhi::ResourceStates::ShaderResource);
            },
            nullptr);
    }

    void
    pipelines::DefaultRenderPipelineSession::addToneMappingFeaturePasses(core::RenderFeatureFrameContext& context) {
        const core::TemporalOutputData& temporal = context.blackboard().get<core::TemporalOutputData>();
        core::ViewportOutputData& viewport = context.blackboard().get<core::ViewportOutputData>();
        const PostProcessPassData& postProcess = context.blackboard().get<PostProcessPassData>();
        const std::uint32_t frameIndex = context.identity().frameSlot.value();
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "Tonemap", FrameGraphPassType::Graphics,
            [&temporal, &viewport](FrameGraphBuilder& builder) {
                builder.readTexture(temporal.color.graphResource, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(temporal.historyWrite.graphResource, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(viewport.color.graphResource, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex,
             framebuffer = postProcess.toneMappingFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, toneMappingPipeline_, frameIndex);
            });
    }

    void pipelines::DefaultRenderPipelineSession::addUiPresentFeaturePasses(core::RenderFeatureFrameContext& context) {
        const core::ViewportOutputData& viewport = context.blackboard().get<core::ViewportOutputData>();
        const core::PresentationInputData& input = context.blackboard().get<core::PresentationInputData>();
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "ImGui overlay", FrameGraphPassType::Graphics,
            [&viewport, &input](FrameGraphBuilder& builder) {
                builder.readTexture(input.fontAtlas.graphResource, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(viewport.color.graphResource, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(input.swapchain.graphResource, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &input](const FrameGraphContext& frameContext) {
                if (input.ui == nullptr) {
                    throw std::logic_error("Presentation Feature requires current ImGui draw data.");
                }
                presentation_.record(*frameContext.commandList, input.imageIndex, input.frameSlot, *input.ui);
            });
        graph.addPass(
            "Present", FrameGraphPassType::Present,
            [&input](FrameGraphBuilder& builder) {
                builder.readTexture(input.swapchain.graphResource, nvrhi::ResourceStates::Present);
            },
            nullptr);
        context.blackboard().set(core::PresentData{.viewport = viewport.color, .swapchain = input.swapchain});
    }

    void pipelines::DefaultRenderPipelineSession::recordShadowPass(nvrhi::ICommandList& commandList,
                                                                   nvrhi::IFramebuffer& framebuffer,
                                                                   std::uint32_t frameIndex, std::uint32_t cascadeIndex,
                                                                   const glm::mat4& lightViewProjection) {
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordShadow(commandList, framebuffer, shadowMapResolution, shadowMapResolution, frameIndex,
                                         cascadeIndex, lightViewProjection);
        }
    }

    void pipelines::DefaultRenderPipelineSession::recordGBufferPass(nvrhi::ICommandList& commandList,
                                                                    nvrhi::IFramebuffer& framebuffer,
                                                                    std::uint32_t frameIndex,
                                                                    const glm::mat4& viewProjection,
                                                                    const glm::mat4& previousViewProjection) {
        const RasterFeatureFrameResources& frame = rasterResources_.frame(frameIndex);
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordGBuffer(commandList, framebuffer, frame.position.width, frame.position.height,
                                          frameIndex, viewProjection, previousViewProjection);
        }
    }

    void pipelines::DefaultRenderPipelineSession::recordFullscreenPass(
        nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
        const nvrhi::GraphicsPipelineHandle& pipeline, std::uint32_t frameIndex,
        const nvrhi::BindingSetHandle& additionalBindingSet) {
        const std::uint32_t width = renderExtent_.width;
        const std::uint32_t height = renderExtent_.height;
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
            .setFramebuffer(&framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
            .addBindingSet(postFxResources_.bindingSet(frameIndex));
        if (additionalBindingSet) {
            state.addBindingSet(additionalBindingSet);
        }
        commandList.setGraphicsState(state);
        commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
    }

    void pipelines::DefaultRenderPipelineSession::recordDirectLightingPass(nvrhi::ICommandList& commandList,
                                                                           nvrhi::IFramebuffer& framebuffer,
                                                                           std::uint32_t frameIndex) {
        if (frameIndex >= directLightingBindingSets_.size() || !directLightingBindingSets_[frameIndex]) {
            throw std::logic_error("Direct-lighting material bindings are unavailable for the frame slot.");
        }
        nvrhi::GraphicsState state;
        state.setPipeline(directLightingPipeline_)
            .setFramebuffer(&framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(renderExtent_.width), static_cast<float>(renderExtent_.height))))
            .addBindingSet(postFxResources_.bindingSet(frameIndex))
            .addBindingSet(directLightingBindingSets_[frameIndex]);
        commandList.setGraphicsState(state);
        commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
    }

    void pipelines::DefaultRenderPipelineSession::recordHistoryCopy(nvrhi::ICommandList& commandList,
                                                                    std::uint32_t frameIndex) {
        const PostFxFrameResources& frame = postFxResources_.frame(frameIndex);
        commandList.copyTexture(frame.history.texture, nvrhi::TextureSlice{}, frame.taaResolved.texture,
                                nvrhi::TextureSlice{});
    }

} // namespace lumin::render

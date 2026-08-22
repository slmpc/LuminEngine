#include "render/level/LevelRenderFrameData.hpp"
#include "render/level/LevelRendererImpl.hpp"

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
        constexpr float atmosphereLuxToRendererRadiance = 1.0e-5f;
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

        glm::vec4 rendererSunRadiance(const scene::DirectionalLight& sun, bool directLightingEnabled) {
            const float directScale =
                directLightingEnabled ? sun.illuminanceLux * atmosphereLuxToRendererRadiance : 0.0f;
            return glm::vec4{sun.color * directScale, 1.0f};
        }
#endif
    } // namespace

    void LevelRenderer::Impl::commitAtmosphereFeature(const core::RenderFrameIdentity& identity) noexcept {
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

    void LevelRenderer::Impl::commitHybridSurfaceFeature(const core::RenderFrameIdentity& identity) noexcept {
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

    void LevelRenderer::Impl::commitGlobalIlluminationFeature(const core::RenderFrameIdentity& identity) noexcept {
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

    void LevelRenderer::Impl::commitGiDenoiserFeature(const core::RenderFrameIdentity& identity) noexcept {
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

    void LevelRenderer::Impl::discardAtmosphereFeature() noexcept {
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

    void LevelRenderer::Impl::discardHybridSurfaceFeature() noexcept {
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

    void LevelRenderer::Impl::discardGlobalIlluminationFeature() noexcept {
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

    void LevelRenderer::Impl::discardGiDenoiserFeature() noexcept {
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

    void LevelRenderer::Impl::addShadowFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        if (!data.settings->shadows.enabled) {
            return;
        }
        FrameGraph& graph = context.frameGraph();
        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            graph.addPass(
                "CSM clear " + std::to_string(cascade), FrameGraphPassType::Transfer,
                [shadow = data.shadows[cascade]](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::CopyDest);
                },
                [texture = data.frame->shadowCascades[cascade].texture](const FrameGraphContext& frameContext) {
                    frameContext.commandList->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, 1.0f,
                                                                       false, 0);
                });
            graph.addPass(
                "CSM cascade " + std::to_string(cascade), FrameGraphPassType::Graphics,
                [shadow = data.shadows[cascade]](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::DepthWrite);
                },
                [this, &data, cascade,
                 framebuffer = data.shadowFramebuffers[cascade]](const FrameGraphContext& frameContext) {
                    recordShadowPass(*frameContext.commandList, *framebuffer, data.frameIndex, cascade,
                                     data.cascades.viewProjections[cascade]);
                });
        }
    }

    void LevelRenderer::Impl::addGBufferFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        FrameGraph& graph = context.frameGraph();
        const bool resetMotion = context.historyAction(core::HistoryDomain::Taa) == core::HistoryAction::FullReset;
        data.previousViewProjection = resetMotion ? data.viewProjection : previousViewProjection_;
        if (modelRenderer_ != nullptr) {
            modelRenderer_->sync(*data.renderWorld, data.frameIndex, resetMotion);
        }
        graph.addPass(
            "G-buffer clear", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color : {data.position, data.normal, data.albedo, data.motion}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::CopyDest);
                }
                builder.writeTexture(data.materialId, nvrhi::ResourceStates::CopyDest);
                builder.writeTexture(data.depth, nvrhi::ResourceStates::CopyDest);
            },
            [&data](const FrameGraphContext& frameContext) {
                const std::array<nvrhi::Color, 4> colors = {
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}, nvrhi::Color{0.0f, 0.0f, 1.0f, 1.0f},
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f}, nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}};
                const std::array<const GpuTexture*, 4> images = {&data.frame->position, &data.frame->normalRoughness,
                                                                 &data.frame->albedo, &data.frame->motion};
                for (std::uint32_t index = 0; index < images.size(); ++index) {
                    frameContext.commandList->clearTextureFloat(images[index]->texture, nvrhi::AllSubresources,
                                                                colors[index]);
                }
                frameContext.commandList->clearTextureUInt(data.frame->materialId.texture, nvrhi::AllSubresources,
                                                           gpu::GpuMaterialIndex::invalidValue);
                frameContext.commandList->clearDepthStencilTexture(data.frame->depth.texture, nvrhi::AllSubresources,
                                                                   true, 1.0f, false, 0);
            });
        graph.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color : {data.position, data.normal, data.albedo, data.motion}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::RenderTarget);
                }
                builder.writeTexture(data.materialId, nvrhi::ResourceStates::RenderTarget);
                builder.writeTexture(data.depth, nvrhi::ResourceStates::DepthWrite);
            },
            [this, &data, framebuffer = data.gbufferFramebuffer](const FrameGraphContext& frameContext) {
                recordGBufferPass(*frameContext.commandList, *framebuffer, data.frameIndex, data.viewProjection,
                                  data.previousViewProjection);
            });
    }

    void LevelRenderer::Impl::addHybridSurfaceFeaturePasses(core::RenderFeatureFrameContext& context) {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        if (!data.hybridPathActive) {
            return;
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
        if (!data.atmosphereLuts || data.frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[data.frameIndex]) {
            throw std::logic_error("Hybrid surface requires the current atmosphere LUT graph and binding set.");
        }

        const bool resetMotion = context.historyAction(core::HistoryDomain::Taa) == core::HistoryAction::FullReset;
        data.previousViewProjection = resetMotion ? data.viewProjection : previousViewProjection_;
        if (modelRenderer_ != nullptr) {
            modelRenderer_->sync(*data.renderWorld, data.frameIndex, resetMotion);
        }
        if (modelRenderer_ == nullptr || modelRenderer_->baseColorTextures().empty() ||
            modelRenderer_->baseColorTextures().size() != modelRenderer_->normalRoughnessTextures().size() ||
            !modelRenderer_->materialSampler()) {
            throw std::logic_error("Hybrid surface requires the shared model material table and texture descriptors.");
        }

        runtime.pendingSequence = context.identity().sequence;
        runtime.pendingScenePlan = runtime.scenePlanner->plan(world::SceneDelta{
            .changes = data.sceneChanges,
            .snapshot = data.renderWorldSnapshot,
        });
        runtime.pendingSceneUpdate = runtime.sceneResources->recordUpdate(
            context.frameGraph(), *runtime.pendingScenePlan, context.identity().frameSlot, true);

        const gpu::GpuScenePreparedUpdate& update = *runtime.pendingSceneUpdate;
        data.hybridSceneDescriptors = runtime.sceneResources->candidateDescriptors(update);
        data.hybridGeometry = runtime.sceneResources->candidateGeometry(update);
        const std::span<const gpu::GpuGeometryFrameGraphResources> geometryResources = update.geometryResources();
        if (geometryResources.size() != data.hybridGeometry.size()) {
            throw std::logic_error("GPU Scene native and FrameGraph geometry arrays do not match.");
        }
        data.hybridVertices.clear();
        data.hybridIndices.clear();
        data.hybridVertices.reserve(geometryResources.size());
        data.hybridIndices.reserve(geometryResources.size());
        for (const gpu::GpuGeometryFrameGraphResources& geometry : geometryResources) {
            data.hybridVertices.push_back(geometry.vertices);
            data.hybridIndices.push_back(geometry.indices);
        }
        data.hybridTlas = update.tlasResource();
        data.hybridInstances = update.instanceRecordsResource();
        const nvrhi::BufferHandle& materialBuffer = modelRenderer_->materialBuffer(data.frameIndex);
        data.materials = context.frameGraph().importBuffer(
            "rt.materials",
            FrameGraphBufferDesc{.size = materialBuffer->getDesc().byteSize,
                                 .buffer = materialBuffer,
                                 .initialState = modelRenderer_->materialBufferInitialState(data.frameIndex),
                                 .finalState = nvrhi::ResourceStates::ShaderResource});
        data.hybridSceneDescriptors.materials = materialBuffer;
        data.hybridMaterials = data.materials;
        data.hybridBaseColorTextures.clear();
        data.hybridNormalRoughnessTextures.clear();
        const std::span<const nvrhi::TextureHandle> baseColorTextures = modelRenderer_->baseColorTextures();
        const std::span<const nvrhi::TextureHandle> normalRoughnessTextures = modelRenderer_->normalRoughnessTextures();
        data.hybridBaseColorTextures.reserve(baseColorTextures.size());
        data.hybridNormalRoughnessTextures.reserve(normalRoughnessTextures.size());
        for (std::size_t index = 0; index < baseColorTextures.size(); ++index) {
            const auto importMaterialTexture = [&](const char* prefix, const nvrhi::TextureHandle& texture) {
                return context.frameGraph().importTexture(
                    std::string{prefix} + std::to_string(index),
                    FrameGraphTextureDesc{.texture = texture,
                                          .initialState = nvrhi::ResourceStates::ShaderResource,
                                          .finalState = nvrhi::ResourceStates::ShaderResource});
            };
            data.hybridBaseColorTextures.push_back(
                importMaterialTexture("rt.material-base-color.", baseColorTextures[index]));
            data.hybridNormalRoughnessTextures.push_back(
                importMaterialTexture("rt.material-normal-roughness.", normalRoughnessTextures[index]));
        }
        data.hybridSceneReadyPass = update.accelerationStructurePass();
        if (!data.hybridSceneReadyPass.isValid()) {
            data.hybridSceneReadyPass = update.uploadPass();
        }

        const scene::DirectionalLight& sun = data.renderWorld->environment().sun;
        const glm::vec3 toSun = -normalizedLightDirection(sun.direction);
        const glm::vec4 sunRadiance = rendererSunRadiance(sun, data.settings->directLighting.enabled);
        const gi::RayTracingEnvironmentBindings environment{
            .atmosphere = atmosphereConsumerBindingSets_[data.frameIndex],
        };
        const gi::RayTracingEnvironmentGraphResources environmentResources{
            .atmosphere = data.atmosphereLuts->resources,
        };
        const gi::RayTracedDiConstants directConstants{
            .inverseViewProjection = glm::inverse(data.viewProjection),
            .previousViewProjection = data.previousViewProjection,
            .cameraPosition = glm::vec4{data.camera->position(), 1.0f},
            .cameraForward = glm::vec4{data.camera->forward(), 0.0f},
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderSize = glm::vec4{static_cast<float>(data.width), static_cast<float>(data.height),
                                    -data.jitter.x / static_cast<float>(data.width),
                                    data.jitter.y / static_cast<float>(data.height)},
            .traceParameters =
                glm::vec4{0.001f, data.camera->farPlane(), data.settings->directLighting.enabled ? 1.0f : 0.0f,
                          static_cast<float>(context.identity().sequence.value())},
        };
        data.hybridSurfacePass = runtime.directLighting->record(
            context.frameGraph(), data.frameIndex, true, directConstants, data.hybridSurface,
            gi::RayTracedGiSceneBindings{
                .descriptors = data.hybridSceneDescriptors,
                .geometry = data.hybridGeometry,
                .baseColorTextures = baseColorTextures,
                .normalRoughnessTextures = normalRoughnessTextures,
                .materialSampler = modelRenderer_->materialSampler(),
            },
            gi::RayTracedGiSceneGraphResources{
                .tlas = data.hybridTlas,
                .instances = data.hybridInstances,
                .materials = data.hybridMaterials,
                .vertices = std::span<const FrameGraphResourceHandle>{data.hybridVertices},
                .indices = std::span<const FrameGraphResourceHandle>{data.hybridIndices},
                .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.hybridBaseColorTextures},
                .normalRoughnessTextures =
                    std::span<const FrameGraphResourceHandle>{data.hybridNormalRoughnessTextures},
                .readyPass = data.hybridSceneReadyPass,
            },
            environment, environmentResources);
#else
        static_cast<void>(context);
#endif
    }

    void LevelRenderer::Impl::addAtmosphereLutFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        if (atmosphereLutGpu_ == nullptr) {
            throw std::logic_error("Atmosphere LUT GPU resources are not initialized.");
        }
        if (pendingAtmosphereSequence_) {
            throw std::logic_error("LevelRenderer already owns a pending atmosphere LUT frame.");
        }

        scene::SceneEnvironment environment = data.renderWorld->environment();
        environment.atmosphere.enabled = environment.atmosphere.enabled && data.settings->atmosphere.enabled;
        const atmosphere::AtmosphereViewInput view =
            atmosphere::makeAtmosphereViewInput(*data.camera, context.identity().extent);
        const atmosphere::AtmosphereLutSignatures signatures =
            atmosphere::makeAtmosphereLutSignatures(environment, view);
        const atmosphere::AtmosphereLutPlan lutPlan = atmosphereLutScheduler_.beginFrame(
            atmosphere::AtmosphereLutFrameInput{context.identity().sequence, signatures, atmosphereForceRebuild_});
        pendingAtmosphereSequence_ = context.identity().sequence;
        const atmosphere::AtmosphereGpuConstants constants = atmosphere::buildAtmosphereGpuConstants(environment, view);
        data.atmosphereLuts = atmosphereLutGpu_->record(context.frameGraph(), data.frameIndex, true, constants,
                                                        atmosphere::makeAtmosphereLutPassPlan(lutPlan));
    }

    void LevelRenderer::Impl::addGlobalIlluminationFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        core::HistoryAction fallbackAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
        fallbackAction =
            core::strongerHistoryAction(fallbackAction, context.historyAction(core::HistoryDomain::NrdSpecular));
        fallbackAction = core::strongerHistoryAction(fallbackAction, context.historyAction(core::HistoryDomain::Sharc));

        if (!shouldUseHybridGi(*data.settings, *data.renderWorld)) {
            if (fallbackAction != core::HistoryAction::Keep) {
                globalIllumination_->invalidateHistory();
            }
            globalIllumination_->addPasses(context.frameGraph(), gi::FrameInfo{
                                                                     *data.renderWorld,
                                                                     data.frameIndex,
                                                                     context.identity().sequence.value(),
                                                                     data.settings->globalIllumination.ssaoEnabled,
                                                                     fallbackAction == core::HistoryAction::FullReset,
                                                                     {data.width, data.height},
                                                                     data.position,
                                                                     data.normal,
                                                                     data.albedo,
                                                                     data.motion,
                                                                     data.depth,
                                                                     data.globalIllumination,
                                                                 });
            return;
        }

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        HybridGiState& runtime = *hybridGi_;
        if (!runtime.pendingSequence || *runtime.pendingSequence != context.identity().sequence ||
            !runtime.pendingScenePlan || !runtime.pendingSceneUpdate || !runtime.directLighting->hasPendingFrame() ||
            runtime.pendingNrdFrame || runtime.sharc->hasPendingFrame() || runtime.rayTracedGi->hasPendingFrame() ||
            runtime.nrd->hasPendingFrame()) {
            throw std::logic_error("Hybrid GI requires the current frame's pending surface transaction.");
        }
        if (!data.atmosphereLuts || data.frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[data.frameIndex]) {
            throw std::logic_error("Hybrid GI requires the current atmosphere LUT graph and binding set.");
        }

        const scene::DirectionalLight& sun = data.renderWorld->environment().sun;
        const glm::vec3 toSun = -normalizedLightDirection(sun.direction);
        const glm::vec4 sunRadiance = rendererSunRadiance(sun, data.settings->directLighting.enabled);
        gi::SharcInvalidationInputs sharcInvalidation{
            .cameraCut = context.changes().containsAny(core::HistoryReason::CameraCut),
            .topologyChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::InstanceTopology),
            .geometryChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::Geometry),
            .materialChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::TransformOrMaterial |
                                                                          world::SceneChangeMask::MaterialBinding),
            .lightingChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::Lighting),
            .atmosphereChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::Atmosphere),
        };
        const core::HistoryAction sharcAction = context.historyAction(core::HistoryDomain::Sharc);
        if (sharcAction == core::HistoryAction::FullReset) {
            sharcInvalidation.topologyChanged = true;
        } else if (sharcAction == core::HistoryAction::SoftReset) {
            sharcInvalidation.materialChanged = true;
        }

        const gi::RayTracingEnvironmentBindings environment{
            .atmosphere = atmosphereConsumerBindingSets_[data.frameIndex],
        };
        const gi::RayTracingEnvironmentGraphResources environmentResources{
            .atmosphere = data.atmosphereLuts->resources,
        };
        const gi::SharcUpdateSceneBindings sceneBindings{
            .descriptors = data.hybridSceneDescriptors,
            .geometry = data.hybridGeometry,
            .baseColorTextures = modelRenderer_->baseColorTextures(),
            .normalRoughnessTextures = modelRenderer_->normalRoughnessTextures(),
            .materialSampler = modelRenderer_->materialSampler(),
        };
        const gi::SharcUpdateSceneGraphResources sceneGraphResources{
            .tlas = data.hybridTlas,
            .instances = data.hybridInstances,
            .materials = data.hybridMaterials,
            .vertices = std::span<const FrameGraphResourceHandle>{data.hybridVertices},
            .indices = std::span<const FrameGraphResourceHandle>{data.hybridIndices},
            .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.hybridBaseColorTextures},
            .normalRoughnessTextures = std::span<const FrameGraphResourceHandle>{data.hybridNormalRoughnessTextures},
            .readyPass = data.hybridSceneReadyPass,
        };
        const gi::SharcFrameParameters sharcFrame{
            .cameraPosition = glm::vec4{data.camera->position(), 1.0f},
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderWidth = data.width,
            .renderHeight = data.height,
            .minTraceDistance = 0.001f,
            .maxTraceDistance = data.camera->farPlane(),
        };
        if (runtime.sharcEnabled) {
            data.sharcRecord = runtime.sharc->record(
                context.frameGraph(), data.frameIndex, true, sharcFrame, sharcInvalidation, environment,
                environmentResources,
                gi::SharcUpdateFrameGraphInputs{data.hybridSurface.worldPositionHitT,
                                                data.hybridSurface.normalRoughness, data.hybridSurface.albedoMetallic},
                sceneBindings, sceneGraphResources);
        }

        const core::HistoryAction diffuseAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
        const core::HistoryAction specularAction = context.historyAction(core::HistoryDomain::NrdSpecular);
        const bool resetNrdMotion = !hasSubmittedFrame_ || diffuseAction == core::HistoryAction::FullReset ||
                                    specularAction == core::HistoryAction::FullReset;
        const glm::vec2 previousJitter = resetNrdMotion ? data.jitter : previousJitter_;
        const glm::vec2 currentEffectiveJitter{-data.jitter.x / static_cast<float>(data.width),
                                               data.jitter.y / static_cast<float>(data.height)};
        const glm::vec2 previousEffectiveJitter{-previousJitter.x / static_cast<float>(data.width),
                                                previousJitter.y / static_cast<float>(data.height)};
        const glm::vec2 effectiveJitterDelta = currentEffectiveJitter - previousEffectiveJitter;
        const gi::RayTracedGiConstants rayTracingConstants{
            .cameraPosition = glm::vec4{data.camera->position(), 1.0f},
            .cameraForward = glm::vec4{data.camera->forward(), 0.0f},
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderSize = glm::vec4{static_cast<float>(data.width), static_cast<float>(data.height),
                                    effectiveJitterDelta.x, effectiveJitterDelta.y},
            .traceParameters = glm::vec4{0.001f, data.camera->farPlane(),
                                         static_cast<float>(context.identity().sequence.value()), nrdDenoisingRange},
        };
        data.rayTracedSignals = runtime.rayTracedGi->record(
            context.frameGraph(), data.frameIndex, true, rayTracingConstants,
            gi::RayTracedGiFrameGraphInputs{data.hybridSurface.worldPositionHitT, data.hybridSurface.normalRoughness,
                                            data.hybridSurface.albedoMetallic, data.hybridSurface.motion},
            gi::RayTracedGiSceneBindings{
                .descriptors = data.hybridSceneDescriptors,
                .geometry = data.hybridGeometry,
                .baseColorTextures = modelRenderer_->baseColorTextures(),
                .normalRoughnessTextures = modelRenderer_->normalRoughnessTextures(),
                .materialSampler = modelRenderer_->materialSampler(),
            },
            gi::RayTracedGiSceneGraphResources{
                .tlas = data.hybridTlas,
                .instances = data.hybridInstances,
                .materials = data.hybridMaterials,
                .vertices = std::span<const FrameGraphResourceHandle>{data.hybridVertices},
                .indices = std::span<const FrameGraphResourceHandle>{data.hybridIndices},
                .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.hybridBaseColorTextures},
                .normalRoughnessTextures =
                    std::span<const FrameGraphResourceHandle>{data.hybridNormalRoughnessTextures},
                .readyPass = data.hybridSceneReadyPass,
            },
            environment, environmentResources, data.sharcRecord ? &*data.sharcRecord : nullptr);
        if (data.sharcRecord) {
            static_cast<void>(runtime.sharc->recordStatisticsReadback(context.frameGraph(), *data.sharcRecord,
                                                                      data.rayTracedSignals->tracePass));
        }
        data.hybridGiActive = true;
#endif
    }

    void LevelRenderer::Impl::addGiDenoiserFeaturePasses(core::RenderFeatureFrameContext& context) {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        if (!data.hybridGiActive) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (!runtime.pendingSequence || *runtime.pendingSequence != context.identity().sequence ||
            !runtime.pendingSceneUpdate || !data.rayTracedSignals || runtime.pendingNrdFrame) {
            throw std::logic_error("NRD requires the current frame's RT GI transaction.");
        }

        const gi::RayTracedGiSignalResources& signals = runtime.rayTracedGi->signals(data.frameIndex);
        const gi::RayTracedGiGraphSignals& graphSignals = *data.rayTracedSignals;
        nvrhi::TextureHandle diffuseInput = signals.diffuseRadianceHitDistance;
        nvrhi::TextureHandle specularInput = signals.specularRadianceHitDistance;
        FrameGraphResourceHandle diffuseGraphInput = graphSignals.diffuseRadianceHitDistance;
        FrameGraphResourceHandle specularGraphInput = graphSignals.specularRadianceHitDistance;
        FrameGraphPassHandle signalReadyPass = graphSignals.tracePass;

        if (data.settings->globalIllumination.nrdEnabled) {
            const core::HistoryAction diffuseAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
            const core::HistoryAction specularAction = context.historyAction(core::HistoryDomain::NrdSpecular);
            const bool resetCameraHistory = !hasSubmittedFrame_ || diffuseAction == core::HistoryAction::FullReset ||
                                            specularAction == core::HistoryAction::FullReset;
            const glm::mat4& previousView = resetCameraHistory ? data.view : previousView_;
            const glm::mat4& previousProjection = resetCameraHistory ? data.projection : previousProjection_;
            const glm::vec2 previousJitter = resetCameraHistory ? data.jitter : previousJitter_;

            gi::NrdCameraData cameraData;
            cameraData.viewToClip = matrixElements(data.projection);
            cameraData.viewToClipPrevious = matrixElements(previousProjection);
            cameraData.worldToView = matrixElements(data.view);
            cameraData.worldToViewPrevious = matrixElements(previousView);
            cameraData.jitter = {data.jitter.x, data.jitter.y};
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
        const TextureFrameResources& frame = *data.frame;
        const FrameGraphPassHandle compositePass = runtime.composite->record(
            context.frameGraph(),
            gi::GiCompositeFrameParameters{
                .frameSlot = context.identity().frameSlot,
                .extent = context.identity().extent,
                .cameraPosition = data.camera->position(),
                .frameSlotFenceWaited = true,
            },
            gi::GiCompositeResources{
                .diffuseRadianceHitDistance = diffuseInput,
                .specularRadianceHitDistance = specularInput,
                .position = frame.position.texture,
                .normalRoughness = frame.normalRoughness.texture,
                .albedoMetallic = frame.albedo.texture,
                .materialId = frame.materialId.texture,
                .materials = data.hybridSceneDescriptors.materials,
                // Hybrid uses TAA's physical target as an indirect-light scratch texture. The final
                // indirect is staged in TAA's physical target before the Hybrid lighting composite.
                .globalIllumination =
                    data.hybridPathActive ? frame.taaResolved.texture : frame.globalIllumination.texture,
            },
            gi::GiCompositeGraphResources{
                .diffuseRadianceHitDistance = diffuseGraphInput,
                .specularRadianceHitDistance = specularGraphInput,
                .position = data.position,
                .normalRoughness = data.normal,
                .albedoMetallic = data.albedo,
                .materialId = data.materialId,
                .materials = data.hybridMaterials,
                .globalIllumination = data.hybridPathActive ? data.taaResolved : data.globalIllumination,
            },
            signalReadyPass);
        if (data.hybridPathActive) {
            const gi::RayTracedDiFrameResources& surface = runtime.directLighting->signals(data.frameIndex);
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
                                                      .indirectRadiance = frame.taaResolved.texture,
                                                      .output = frame.lighting.texture,
                                                  },
                                                  gi::HybridLightingCompositeGraphResources{
                                                      .directRadiance = data.hybridSurface.directRadiance,
                                                      .indirectRadiance = data.taaResolved,
                                                      .output = data.lighting,
                                                  },
                                                  compositePass);
            static_cast<void>(lightingPass);
            data.taaInput = data.lighting;
        } else {
            data.taaInput = data.lighting;
        }
#else
        static_cast<void>(context);
#endif
    }

    void LevelRenderer::Impl::addSkyCompositeFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        // RT primary miss 已经把 atmosphere environment 写入 directRadiance；Hybrid 的 compute composite
        // 只需等待 GI denoiser，不再声明旧的 raster sky framebuffer/CSM 输入。
        if (data.hybridPathActive) {
            if (data.hybridGiActive) {
                return;
            }
            if (hybridGi_ == nullptr || hybridGi_->lightingComposite == nullptr ||
                hybridGi_->directLighting == nullptr || !data.hybridSurface.directRadiance.isValid() ||
                !data.hybridSurfacePass.isValid()) {
                throw std::logic_error("Hybrid direct-only composite requires the RTDI surface pass.");
            }

            const gi::RayTracedDiFrameResources& surface = hybridGi_->directLighting->signals(data.frameIndex);
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
                                                         .indirectRadiance = data.frame->globalIllumination.texture,
                                                         .output = data.frame->lighting.texture,
                                                     },
                                                     gi::HybridLightingCompositeGraphResources{
                                                         .directRadiance = data.hybridSurface.directRadiance,
                                                         .indirectRadiance = data.globalIllumination,
                                                         .output = data.lighting,
                                                     },
                                                     data.hybridSurfacePass));
            data.taaInput = data.lighting;
            return;
        }
#endif
        FrameGraph& graph = context.frameGraph();
        if (!data.atmosphereLuts || data.frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[data.frameIndex]) {
            throw std::logic_error("Sky composite requires the atmosphere LUT record from the current frame.");
        }
        graph.addPass(
            "Procedural sky clear", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.writeTexture(data.lighting, nvrhi::ResourceStates::CopyDest);
            },
            [texture = data.frame->lighting.texture](const FrameGraphContext& frameContext) {
                frameContext.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                            nvrhi::Color{0.035f, 0.04f, 0.05f, 1.0f});
            });
        graph.addPass(
            "Procedural sky", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.globalIllumination, nvrhi::ResourceStates::ShaderResource);
                for (const FrameGraphResourceHandle lut : data.atmosphereLuts->resources.textures) {
                    builder.readTexture(lut, nvrhi::ResourceStates::ShaderResource);
                }
                builder.read(data.atmosphereLuts->resources.constants, nvrhi::ResourceStates::ConstantBuffer);
                builder.writeTexture(data.lighting, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.lightingFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, pipelines_.sky(), data.frameIndex,
                                     atmosphereConsumerBindingSets_[data.frameIndex]);
            });
    }

    void LevelRenderer::Impl::addDirectLightingFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (data.hybridPathActive) {
            return;
        }
#endif
        if (modelRenderer_ == nullptr) {
            return;
        }
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "Deferred lighting", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input :
                     {data.position, data.normal, data.albedo, data.globalIllumination, data.materialId}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                builder.read(data.materials, nvrhi::ResourceStates::ShaderResource);
                for (FrameGraphResourceHandle shadow : data.shadows) {
                    builder.readTexture(shadow, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(data.lighting, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.lightingFramebuffer](const FrameGraphContext& frameContext) {
                recordDirectLightingPass(*frameContext.commandList, *framebuffer, data.frameIndex);
            });
    }

    void LevelRenderer::Impl::addTemporalAaFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        FrameGraph& graph = context.frameGraph();
        const core::HistoryAction action = context.historyAction(core::HistoryDomain::Taa);
        data.uniforms.renderOptions.x =
            action == core::HistoryAction::Keep && textures_.historyValid(data.historyReadIndex) ? 1.0f : 0.0f;
        textures_.updatePostProcessUniforms(data.frameIndex, data.uniforms);

        graph.addPass(
            "TAA clear", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.writeTexture(data.taaResolved, nvrhi::ResourceStates::CopyDest);
            },
            [texture = data.frame->taaResolved.texture](const FrameGraphContext& frameContext) {
                frameContext.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                            nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f});
            });
        graph.addPass(
            "TAA resolve", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input : {data.taaInput, data.motion, data.historyRead}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(data.taaResolved, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.taaFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, pipelines_.taa(), data.frameIndex);
            });
        graph.addPass(
            "TAA history copy", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.taaResolved, nvrhi::ResourceStates::CopySource);
                builder.writeTexture(data.historyWrite, nvrhi::ResourceStates::CopyDest);
            },
            [this, &data](const FrameGraphContext& frameContext) {
                recordHistoryCopy(*frameContext.commandList, data.frameIndex);
            });
        graph.addPass(
            "TAA history ready", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.historyWrite, nvrhi::ResourceStates::ShaderResource);
            },
            nullptr);
    }

    void LevelRenderer::Impl::addToneMappingFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "Tonemap", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.taaResolved, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(data.historyWrite, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(data.viewportOutput, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.tonemapFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, pipelines_.tonemap(), data.frameIndex);
            });
    }

    void LevelRenderer::Impl::addUiPresentFeaturePasses(core::RenderFeatureFrameContext& context) {
        LevelRenderFrameData& data = context.blackboard().get<LevelRenderFrameData>();
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "ImGui overlay", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.imguiFont, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(data.viewportOutput, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(data.swap, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data](const FrameGraphContext& frameContext) {
                imgui_.record(*frameContext.commandList, data.imageIndex, data.frameIndex);
            });
        graph.addPass(
            "Present", FrameGraphPassType::Present,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.swap, nvrhi::ResourceStates::Present);
            },
            nullptr);
    }

    void LevelRenderer::Impl::recordShadowPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                               std::uint32_t frameIndex, std::uint32_t cascadeIndex,
                                               const glm::mat4& lightViewProjection) {
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordShadow(commandList, framebuffer, shadowMapResolution, shadowMapResolution, frameIndex,
                                         cascadeIndex, lightViewProjection);
        }
    }

    void LevelRenderer::Impl::recordGBufferPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                                std::uint32_t frameIndex, const glm::mat4& viewProjection,
                                                const glm::mat4& previousViewProjection) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordGBuffer(commandList, framebuffer, frame.position.width, frame.position.height,
                                          frameIndex, viewProjection, previousViewProjection);
        }
    }

    void LevelRenderer::Impl::recordFullscreenPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                                   const nvrhi::GraphicsPipelineHandle& pipeline,
                                                   std::uint32_t frameIndex,
                                                   const nvrhi::BindingSetHandle& additionalBindingSet) {
        const std::uint32_t width = renderExtent_.width;
        const std::uint32_t height = renderExtent_.height;
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
            .setFramebuffer(&framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
            .addBindingSet(textures_.bindingSet(frameIndex));
        if (additionalBindingSet) {
            state.addBindingSet(additionalBindingSet);
        }
        commandList.setGraphicsState(state);
        commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
    }

    void LevelRenderer::Impl::recordDirectLightingPass(nvrhi::ICommandList& commandList,
                                                       nvrhi::IFramebuffer& framebuffer, std::uint32_t frameIndex) {
        if (frameIndex >= directLightingBindingSets_.size() || !directLightingBindingSets_[frameIndex]) {
            throw std::logic_error("Direct-lighting material bindings are unavailable for the frame slot.");
        }
        nvrhi::GraphicsState state;
        state.setPipeline(pipelines_.deferredLighting())
            .setFramebuffer(&framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(renderExtent_.width), static_cast<float>(renderExtent_.height))))
            .addBindingSet(textures_.bindingSet(frameIndex))
            .addBindingSet(directLightingBindingSets_[frameIndex]);
        commandList.setGraphicsState(state);
        commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
    }

    void LevelRenderer::Impl::recordHistoryCopy(nvrhi::ICommandList& commandList, std::uint32_t frameIndex) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        commandList.copyTexture(frame.history.texture, nvrhi::TextureSlice{}, frame.taaResolved.texture,
                                nvrhi::TextureSlice{});
    }

} // namespace lumin::render

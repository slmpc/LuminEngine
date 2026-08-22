#include "render/level/LevelRenderFrameData.hpp"
#include "render/level/LevelRendererImpl.hpp"

#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/Camera.hpp"
#include "scene/Level.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/trigonometric.hpp>

namespace lumin::render {
    namespace {
        glm::vec3 normalizedLightDirection(glm::vec3 direction) {
            if (glm::dot(direction, direction) < 1.0e-6f) {
                direction = glm::vec3{-0.45f, -0.8f, -0.35f};
            }
            return glm::normalize(direction);
        }

        float halton(std::uint32_t index, std::uint32_t base) {
            float result = 0.0f;
            float fraction = 1.0f;
            while (index > 0) {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(index % base);
                index /= base;
            }
            return result;
        }

        LevelCascadeShadowData calculateCascadeShadows(const scene::Camera& camera, float aspectRatio,
                                                       glm::vec3 lightDirection, const ShadowSettings& settings) {
            LevelCascadeShadowData result;
            std::array<float, shadowCascadeCount> splits{};
            const float splitLambda = std::clamp(settings.splitLambda, 0.0f, 1.0f);
            const float shadowFar = std::clamp(settings.maxDistance, camera.nearPlane() + 0.001f, camera.farPlane());
            const float clipRange = shadowFar - camera.nearPlane();
            const float clipRatio = shadowFar / camera.nearPlane();
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(shadowCascadeCount);
                const float logarithmic = camera.nearPlane() * std::pow(clipRatio, fraction);
                const float uniform = camera.nearPlane() + clipRange * fraction;
                splits[cascade] = splitLambda * logarithmic + (1.0f - splitLambda) * uniform;
                result.splits[cascade] = splits[cascade];
            }

            lightDirection = normalizedLightDirection(lightDirection);
            const glm::vec3 cameraForward = camera.forward();
            const glm::vec3 cameraRight = camera.right();
            const glm::vec3 cameraUp = camera.up();
            const float tanHalfFov = std::tan(glm::radians(camera.fieldOfViewDegrees()) * 0.5f);
            const glm::vec3 upReference = std::abs(glm::dot(lightDirection, glm::vec3{0.0f, 1.0f, 0.0f})) > 0.95f
                                              ? glm::vec3{0.0f, 0.0f, 1.0f}
                                              : glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::vec3 lightRight = glm::normalize(glm::cross(lightDirection, upReference));
            const glm::vec3 lightUp = glm::normalize(glm::cross(lightRight, lightDirection));

            float sliceNear = camera.nearPlane();
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const float sliceFar = splits[cascade];
                const float nearHalfHeight = tanHalfFov * sliceNear;
                const float nearHalfWidth = nearHalfHeight * aspectRatio;
                const float farHalfHeight = tanHalfFov * sliceFar;
                const float farHalfWidth = farHalfHeight * aspectRatio;
                const glm::vec3 nearCenter = camera.position() + cameraForward * sliceNear;
                const glm::vec3 farCenter = camera.position() + cameraForward * sliceFar;
                const std::array<glm::vec3, 8> corners = {
                    nearCenter - cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,
                    nearCenter + cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,
                    nearCenter + cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
                    nearCenter - cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
                    farCenter - cameraRight * farHalfWidth - cameraUp * farHalfHeight,
                    farCenter + cameraRight * farHalfWidth - cameraUp * farHalfHeight,
                    farCenter + cameraRight * farHalfWidth + cameraUp * farHalfHeight,
                    farCenter - cameraRight * farHalfWidth + cameraUp * farHalfHeight,
                };

                glm::vec3 center{0.0f};
                for (const glm::vec3& corner : corners) {
                    center += corner;
                }
                center /= static_cast<float>(corners.size());

                float radius = 0.0f;
                for (const glm::vec3& corner : corners) {
                    radius = std::max(radius, glm::length(corner - center));
                }
                radius = std::ceil(radius * 16.0f) / 16.0f;
                const float texelSize = (2.0f * radius) / static_cast<float>(shadowMapResolution);
                const float centerRight = glm::dot(center, lightRight);
                const float centerUp = glm::dot(center, lightUp);
                const float snappedRight = std::floor(centerRight / texelSize + 0.5f) * texelSize;
                const float snappedUp = std::floor(centerUp / texelSize + 0.5f) * texelSize;
                const glm::vec3 snappedCenter =
                    center + lightRight * (snappedRight - centerRight) + lightUp * (snappedUp - centerUp);

                const float casterMargin = std::max(25.0f, radius * 0.5f);
                const float lightDistance = radius + casterMargin;
                const glm::mat4 lightView =
                    glm::lookAt(snappedCenter - lightDirection * lightDistance, snappedCenter, lightUp);
                const glm::mat4 lightProjection =
                    glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.1f, 2.0f * (radius + casterMargin));
                result.viewProjections[cascade] = lightProjection * lightView;
                sliceNear = sliceFar;
            }
            return result;
        }

        FrameGraphTextureDesc textureDesc(const GpuTexture& image,
                                          nvrhi::ResourceStates initialState = nvrhi::ResourceStates::Unknown) {
            FrameGraphTextureDesc desc;
            desc.texture = image.texture;
            desc.initialState = initialState;
            desc.finalState = nvrhi::ResourceStates::ShaderResource;
            return desc;
        }

        nvrhi::FramebufferHandle createFramebuffer(nvrhi::IDevice& device, const nvrhi::FramebufferDesc& desc) {
            nvrhi::FramebufferHandle framebuffer = device.createFramebuffer(desc);
            if (!framebuffer) {
                throw std::runtime_error("Failed to create an NvRHI LevelRenderer framebuffer.");
            }
            return framebuffer;
        }
    } // namespace

    LevelRenderer::Impl::RecordedFrameState
    LevelRenderer::Impl::recordCommandList(nvrhi::ICommandList& commandList, const core::RenderFrameIdentity& identity,
                                           const scene::Camera& camera, const RenderSettings& settings,
                                           const core::UiDrawPacket& uiDrawPacket, world::SceneChangeMask sceneChanges,
                                           const core::FrameChangeSet& changes) {
        if (!identity.isValid()) {
            throw std::invalid_argument("LevelRenderer requires a valid render frame identity.");
        }
        const std::uint32_t frameIndex = identity.frameSlot.value();
        const std::uint32_t imageIndex = identity.swapImage.value();
        const std::uint32_t width = identity.extent.width;
        const std::uint32_t height = identity.extent.height;
        const float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
        const glm::mat4 view = camera.viewMatrix();
        const glm::mat4 unjitteredProjection = camera.projectionMatrix(aspectRatio);
        const glm::mat4 unjitteredViewProjection = unjitteredProjection * view;
        glm::mat4 projection = unjitteredProjection;
        glm::vec2 jitter{0.0f};
        if (settings.temporalAa.enabled) {
            const std::uint32_t jitterIndex = static_cast<std::uint32_t>(identity.sequence.value() % 8U) + 1U;
            jitter = glm::vec2{halton(jitterIndex, 2U) - 0.5f, halton(jitterIndex, 3U) - 0.5f};
            projection[2][0] += (2.0f * jitter.x) / static_cast<float>(width);
            projection[2][1] += (2.0f * jitter.y) / static_cast<float>(height);
        }
        const glm::mat4 viewProjection = projection * view;
        const world::RenderWorldSnapshotPtr renderWorld = renderWorld_.snapshot();
        if (renderWorld == nullptr) {
            throw std::logic_error("LevelRenderer cannot record without a render-world snapshot.");
        }

        const scene::DirectionalLight& sun = renderWorld->environment().sun;
        const glm::vec3 cameraForward = camera.forward();
        const glm::vec3 lightDirection = normalizedLightDirection(sun.direction);
        const LevelCascadeShadowData cascades =
            calculateCascadeShadows(camera, aspectRatio, lightDirection, settings.shadows);
        const std::uint32_t historyReadIndex =
            (frameIndex + TextureManager::maxFramesInFlight - 1) % TextureManager::maxFramesInFlight;
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        const TextureFrameResources& historyReadFrame = textures_.frame(historyReadIndex);

        LevelRenderFrameData data;
        data.renderWorldSnapshot = renderWorld;
        data.renderWorld = renderWorld.get();
        data.camera = &camera;
        data.settings = &settings;
        data.uiDrawPacket = &uiDrawPacket;
        data.frame = &frame;
        data.frameIndex = frameIndex;
        data.imageIndex = imageIndex;
        data.historyReadIndex = historyReadIndex;
        data.width = width;
        data.height = height;
        data.sceneChanges = sceneChanges;
        data.view = view;
        data.projection = unjitteredProjection;
        data.viewProjection = viewProjection;
        data.previousViewProjection = previousViewProjection_;
        data.jitter = jitter;
        data.cascades = cascades;
        data.hybridPathActive =
            renderPipeline_ != nullptr && activePipelineKind_ == pipelines::DefaultRenderPipelineKind::Hybrid;
        data.uniforms.inverseViewProjection = glm::inverse(unjitteredViewProjection);
        data.uniforms.viewProjection = viewProjection;
        data.uniforms.cascadeViewProjections = cascades.viewProjections;
        data.uniforms.cascadeSplits = cascades.splits;
        data.uniforms.cameraPosition = glm::vec4{camera.position(), 1.0f};
        data.uniforms.cameraForward = glm::vec4{cameraForward, 0.0f};
        data.uniforms.lightDirection = glm::vec4{lightDirection, settings.directLighting.enabled ? 1.0f : 0.0f};
        data.uniforms.renderSize = glm::vec4{static_cast<float>(width), static_cast<float>(height),
                                             1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
        data.uniforms.renderOptions = glm::vec4{0.0f, settings.globalIllumination.ssaoEnabled ? 1.0f : 0.0f,
                                                settings.shadows.enabled && sun.castsShadows ? 1.0f : 0.0f,
                                                settings.temporalAa.enabled ? 1.0f : 0.0f};
        data.uniforms.tonemapOptions.x = settings.toneMapping.exposure;
        data.uniforms.tonemapOptions.y = context_.swapchainIsSrgb() ? 1.0f : 0.0f;
        data.uniforms.ambientOcclusionOptions =
            glm::vec4{static_cast<float>(settings.globalIllumination.ambientOcclusionMode),
                      std::max(settings.globalIllumination.ambientOcclusionRadius, 0.05f),
                      std::max(settings.globalIllumination.ambientOcclusionStrength, 0.0f),
                      std::clamp(settings.globalIllumination.ambientOcclusionBias, 0.0f, 0.5f)};

        frameGraph_.reset();
        const nvrhi::ResourceStates frameInitialState = frameResourcesInitialized_[frameIndex]
                                                            ? nvrhi::ResourceStates::ShaderResource
                                                            : nvrhi::ResourceStates::Common;
        const nvrhi::ResourceStates depthInitialState =
            frameResourcesInitialized_[frameIndex] ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::Common;
        if (!data.hybridPathActive) {
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                data.shadows[cascade] =
                    frameGraph_.importTexture("shadow.cascade" + std::to_string(cascade),
                                              textureDesc(frame.shadowCascades[cascade], frameInitialState));
            }
            data.position =
                frameGraph_.importTexture("gbuffer.position", textureDesc(frame.position, frameInitialState));
            data.normal =
                frameGraph_.importTexture("gbuffer.normal", textureDesc(frame.normalRoughness, frameInitialState));
            data.albedo = frameGraph_.importTexture("gbuffer.albedo", textureDesc(frame.albedo, frameInitialState));
            data.motion = frameGraph_.importTexture("gbuffer.motion", textureDesc(frame.motion, frameInitialState));
            FrameGraphTextureDesc materialIdDesc = textureDesc(frame.materialId, frameInitialState);
            materialIdDesc.finalState = nvrhi::ResourceStates::ShaderResource;
            data.materialId = frameGraph_.importTexture("gbuffer.material-id", materialIdDesc);
            if (modelRenderer_ != nullptr) {
                const nvrhi::BufferHandle& materialBuffer = modelRenderer_->materialBuffer(frameIndex);
                data.materials = frameGraph_.importBuffer(
                    "gpu-scene.materials",
                    FrameGraphBufferDesc{.size = materialBuffer->getDesc().byteSize,
                                         .buffer = materialBuffer,
                                         .initialState = modelRenderer_->materialBufferInitialState(frameIndex),
                                         .finalState = nvrhi::ResourceStates::ShaderResource});
            }
            FrameGraphTextureDesc depthDesc = textureDesc(frame.depth, depthInitialState);
            depthDesc.finalState = nvrhi::ResourceStates::DepthWrite;
            data.depth = frameGraph_.importTexture("gbuffer.depth", depthDesc);
        }
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        else {
            if (hybridGi_ == nullptr || hybridGi_->directLighting == nullptr) {
                throw std::logic_error("Hybrid render topology requires the RT surface runtime.");
            }
            const gi::RayTracedDiFrameResources& surface = hybridGi_->directLighting->signals(frameIndex);
            data.position = frameGraph_.importTexture(
                "rt.surface.world-position", textureDesc(GpuTexture{surface.worldPositionHitT}, frameInitialState));
            data.normal = frameGraph_.importTexture(
                "rt.surface.normal-roughness", textureDesc(GpuTexture{surface.normalRoughness}, frameInitialState));
            data.albedo = frameGraph_.importTexture("rt.surface.albedo-metallic",
                                                    textureDesc(GpuTexture{surface.albedoMetallic}, frameInitialState));
            data.materialId = frameGraph_.importTexture("rt.surface.material-id",
                                                        textureDesc(GpuTexture{surface.materialId}, frameInitialState));
            data.motion = frameGraph_.importTexture("rt.surface.motion",
                                                    textureDesc(GpuTexture{surface.motion}, frameInitialState));
            data.depth = frameGraph_.importTexture("rt.surface.view-z",
                                                   textureDesc(GpuTexture{surface.viewZ}, frameInitialState));
            data.hybridSurface.worldPositionHitT = data.position;
            data.hybridSurface.normalRoughness = data.normal;
            data.hybridSurface.albedoMetallic = data.albedo;
            data.hybridSurface.materialId = data.materialId;
            data.hybridSurface.viewZ = data.depth;
            data.hybridSurface.motion = data.motion;
            data.hybridSurface.visibilityMask = frameGraph_.importTexture(
                "rt.surface.visibility", textureDesc(GpuTexture{surface.visibilityMask}, frameInitialState));
        }
#endif
        data.globalIllumination = frameGraph_.importTexture("global-illumination.output",
                                                            textureDesc(frame.globalIllumination, frameInitialState));
        data.lighting = frameGraph_.importTexture(data.hybridPathActive ? "rt.surface.direct-radiance" : "lighting.hdr",
                                                  textureDesc(frame.lighting, frameInitialState));
        data.taaInput = data.lighting;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (data.hybridPathActive) {
            data.hybridSurface.directRadiance = data.globalIllumination;
        }
#endif
        data.taaResolved = frameGraph_.importTexture("taa.resolved", textureDesc(frame.taaResolved, frameInitialState));
        data.historyRead = frameGraph_.importTexture(
            "taa.history.read", textureDesc(historyReadFrame.history, textures_.historyInitialState(historyReadIndex)));
        data.historyWrite = frameGraph_.importTexture(
            "taa.history.write", textureDesc(frame.history, textures_.historyInitialState(frameIndex)));

        FrameGraphTextureDesc viewportDesc =
            textureDesc(viewportOutput_, viewportOutputInitialized_ ? nvrhi::ResourceStates::ShaderResource
                                                                    : nvrhi::ResourceStates::Common);
        viewportDesc.finalState = nvrhi::ResourceStates::ShaderResource;
        data.viewportOutput = frameGraph_.importTexture("viewport.output", viewportDesc);

        FrameGraphTextureDesc swapDesc;
        swapDesc.texture = context_.swapchainTextures().at(imageIndex);
        swapDesc.initialState = context_.swapchainTextureInitialState(imageIndex);
        if (swapDesc.initialState == nvrhi::ResourceStates::Unknown) {
            swapDesc.initialState = nvrhi::ResourceStates::Common;
        }
        swapDesc.finalState = nvrhi::ResourceStates::Present;
        data.swap = frameGraph_.importTexture("swapchain.color", swapDesc);
        FrameGraphTextureDesc fontDesc;
        fontDesc.texture = presentation_.fontTexture();
        fontDesc.initialState = presentation_.fontTextureInitialState();
        fontDesc.finalState = nvrhi::ResourceStates::ShaderResource;
        data.imguiFont = frameGraph_.importTexture("imgui.font", fontDesc);

        nvrhi::IDevice& device = *context_.rhiDevice();
        if (!data.hybridPathActive) {
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                data.shadowFramebuffers[cascade] = createFramebuffer(
                    device, nvrhi::FramebufferDesc().setDepthAttachment(frame.shadowCascades[cascade].texture));
            }
            nvrhi::FramebufferDesc gbufferDesc;
            gbufferDesc.addColorAttachment(frame.position.texture)
                .addColorAttachment(frame.normalRoughness.texture)
                .addColorAttachment(frame.albedo.texture)
                .addColorAttachment(frame.motion.texture)
                .addColorAttachment(frame.materialId.texture)
                .setDepthAttachment(frame.depth.texture);
            data.gbufferFramebuffer = createFramebuffer(device, gbufferDesc);
            data.lightingFramebuffer =
                createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(frame.lighting.texture));
        }
        data.taaFramebuffer =
            createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(frame.taaResolved.texture));
        data.tonemapFramebuffer =
            createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(viewportOutput_.texture));

        core::RenderBlackboard blackboard;
        blackboard.set(std::move(data));
        renderPipeline_->prepareFrame(identity, camera.cutEpoch(), changes, frameGraph_, blackboard);
        frameGraph_.execute(FrameGraphContext{&commandList, nullptr, frameIndex});
        bool usedHybridGlobalIllumination = false;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        usedHybridGlobalIllumination = blackboard.get<LevelRenderFrameData>().hybridGiActive;
#endif
        return RecordedFrameState{viewProjection,
                                  view,
                                  unjitteredProjection,
                                  jitter,
                                  featureConfiguration(settings),
                                  usedHybridGlobalIllumination};
    }

} // namespace lumin::render

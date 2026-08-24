#include "render/level/FeatureFrameData.hpp"
#include "render/pipelines/default/DefaultRenderPipelineSession.hpp"

#include "render/platform/vulkan/VulkanContext.hpp"
#include "render/resources/FrameGraphResourceImporter.hpp"
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

        core::ShadowData calculateCascadeShadows(const core::CameraFrameData& camera, float aspectRatio,
                                                 glm::vec3 lightDirection, const ShadowSettings& settings) {
            core::ShadowData result;
            std::array<float, shadowCascadeCount> splits{};
            const float splitLambda = std::clamp(settings.splitLambda, 0.0f, 1.0f);
            const float shadowFar = std::clamp(settings.maxDistance, camera.nearPlane + 0.001f, camera.farPlane);
            const float clipRange = shadowFar - camera.nearPlane;
            const float clipRatio = shadowFar / camera.nearPlane;
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(shadowCascadeCount);
                const float logarithmic = camera.nearPlane * std::pow(clipRatio, fraction);
                const float uniform = camera.nearPlane + clipRange * fraction;
                splits[cascade] = splitLambda * logarithmic + (1.0f - splitLambda) * uniform;
                result.splits[cascade] = splits[cascade];
            }

            lightDirection = normalizedLightDirection(lightDirection);
            const glm::vec3 cameraForward = glm::vec3{camera.forward};
            const glm::vec3 cameraRight = camera.right;
            const glm::vec3 cameraUp = camera.up;
            const glm::vec3 cameraPosition = glm::vec3{camera.position};
            const float tanHalfFov = std::tan(glm::radians(camera.fieldOfViewDegrees) * 0.5f);
            const glm::vec3 upReference = std::abs(glm::dot(lightDirection, glm::vec3{0.0f, 1.0f, 0.0f})) > 0.95f
                                              ? glm::vec3{0.0f, 0.0f, 1.0f}
                                              : glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::vec3 lightRight = glm::normalize(glm::cross(lightDirection, upReference));
            const glm::vec3 lightUp = glm::normalize(glm::cross(lightRight, lightDirection));

            float sliceNear = camera.nearPlane;
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const float sliceFar = splits[cascade];
                const float nearHalfHeight = tanHalfFov * sliceNear;
                const float nearHalfWidth = nearHalfHeight * aspectRatio;
                const float farHalfHeight = tanHalfFov * sliceFar;
                const float farHalfWidth = farHalfHeight * aspectRatio;
                const glm::vec3 nearCenter = cameraPosition + cameraForward * sliceNear;
                const glm::vec3 farCenter = cameraPosition + cameraForward * sliceFar;
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

        core::TextureFrameData textureFrameData(const GpuTexture& texture, FrameGraphResourceHandle graphResource) {
            return core::TextureFrameData{
                .texture = texture.texture,
                .graphResource = graphResource,
                .readyPass = {},
                .format = texture.format,
                .extent = {texture.width, texture.height},
            };
        }

        core::TextureFrameData textureFrameData(const nvrhi::TextureHandle& texture,
                                                FrameGraphResourceHandle graphResource) {
            const nvrhi::TextureDesc& desc = texture->getDesc();
            return core::TextureFrameData{
                .texture = texture,
                .graphResource = graphResource,
                .readyPass = {},
                .format = desc.format,
                .extent = {desc.width, desc.height},
            };
        }

        nvrhi::FramebufferHandle createFramebuffer(nvrhi::IDevice& device, const nvrhi::FramebufferDesc& desc) {
            nvrhi::FramebufferHandle framebuffer = device.createFramebuffer(desc);
            if (!framebuffer) {
                throw std::runtime_error("Failed to create a default pipeline NvRHI framebuffer.");
            }
            return framebuffer;
        }
    } // namespace

    pipelines::DefaultRenderPipelineSession::RecordedFrameState
    pipelines::DefaultRenderPipelineSession::recordCommandList(nvrhi::ICommandList& commandList,
                                                               const core::RenderFrameIdentity& identity,
                                                               const core::RenderFramePacket& packet,
                                                               const RenderSettings& settings,
                                                               world::SceneChangeMask sceneChanges,
                                                               const core::FrameChangeSet& changes) {
        if (!identity.isValid()) {
            throw std::invalid_argument("Default pipeline session requires a valid render frame identity.");
        }
        const std::uint32_t frameIndex = identity.frameSlot.value();
        const std::uint32_t imageIndex = identity.swapImage.value();
        const std::uint32_t width = identity.extent.width;
        const std::uint32_t height = identity.extent.height;
        const float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
        const core::CameraFrameData& camera = packet.camera;
        const glm::mat4 view = camera.view;
        // resize 防抖期间 packet 的请求尺寸可能领先于当前 GPU 资源，投影必须以实际提交尺寸重新计算。
        const glm::mat4 unjitteredProjection = glm::perspective(
            glm::radians(camera.fieldOfViewDegrees), std::max(aspectRatio, 0.001f), camera.nearPlane, camera.farPlane);
        glm::mat4 projection = unjitteredProjection;
        glm::vec2 jitter{0.0f};
        if (settings.temporalAa.enabled) {
            const std::uint32_t jitterIndex = static_cast<std::uint32_t>(identity.sequence.value() % 8U) + 1U;
            jitter = glm::vec2{halton(jitterIndex, 2U) - 0.5f, halton(jitterIndex, 3U) - 0.5f};
            projection[2][0] += (2.0f * jitter.x) / static_cast<float>(width);
            projection[2][1] += (2.0f * jitter.y) / static_cast<float>(height);
        }
        const glm::mat4 viewProjection = projection * view;
        const world::RenderWorldSnapshotPtr renderWorld = packet.world;
        if (renderWorld == nullptr) {
            throw std::logic_error("Default pipeline session cannot record without a render-world snapshot.");
        }

        const scene::DirectionalLight& sun = renderWorld->environment().sun;
        const glm::vec3 cameraForward = glm::vec3{camera.forward};
        const glm::vec3 lightDirection = normalizedLightDirection(sun.direction);
        core::ShadowData shadows = calculateCascadeShadows(camera, aspectRatio, lightDirection, settings.shadows);
        const std::uint32_t historyReadIndex = (frameIndex + frameSlotCount - 1) % frameSlotCount;
        const RasterFeatureFrameResources& rasterFrame = rasterResources_.frame(frameIndex);
        const PostFxFrameResources& postFxFrame = postFxResources_.frame(frameIndex);
        const PostFxFrameResources& historyReadFrame = postFxResources_.frame(historyReadIndex);

        core::FrameSceneData sceneData{
            .world = renderWorld,
            .camera =
                core::CameraFrameData{
                    .view = view,
                    .projection = unjitteredProjection,
                    .viewProjection = viewProjection,
                    .previousViewProjection = previousViewProjection_,
                    .position = camera.position,
                    .forward = glm::vec4{cameraForward, 0.0f},
                    .right = camera.right,
                    .up = camera.up,
                    .fieldOfViewDegrees = camera.fieldOfViewDegrees,
                    .nearPlane = camera.nearPlane,
                    .farPlane = camera.farPlane,
                    .revision = camera.revision,
                    .jitter = jitter,
                    .cutEpoch = camera.cutEpoch,
                },
            .settings = packet.settings,
            .changes = sceneChanges,
        };
        PostProcessPassData postProcess;
        postProcess.historyReadSlot = historyReadIndex;
        postProcess.uniforms.inverseViewProjection = glm::inverse(viewProjection);
        postProcess.uniforms.viewProjection = viewProjection;
        postProcess.uniforms.cascadeViewProjections = shadows.viewProjections;
        postProcess.uniforms.cascadeSplits = shadows.splits;
        postProcess.uniforms.cameraPosition = camera.position;
        postProcess.uniforms.cameraForward = glm::vec4{cameraForward, 0.0f};
        postProcess.uniforms.lightDirection = glm::vec4{lightDirection, settings.directLighting.enabled ? 1.0f : 0.0f};
        postProcess.uniforms.renderSize =
            glm::vec4{static_cast<float>(width), static_cast<float>(height), 1.0f / static_cast<float>(width),
                      1.0f / static_cast<float>(height)};
        const glm::vec2 currentJitterUv{-jitter.x / static_cast<float>(width), jitter.y / static_cast<float>(height)};
        const glm::vec2 previousJitter = hasSubmittedFrame_ ? previousJitter_ : jitter;
        const glm::vec2 previousJitterUv{-previousJitter.x / static_cast<float>(width),
                                         previousJitter.y / static_cast<float>(height)};
        postProcess.uniforms.temporalOptions =
            glm::vec4{currentJitterUv.x, currentJitterUv.y, previousJitterUv.x, previousJitterUv.y};
        postProcess.uniforms.renderOptions = glm::vec4{0.0f, settings.globalIllumination.ssaoEnabled ? 1.0f : 0.0f,
                                                       settings.shadows.enabled && sun.castsShadows ? 1.0f : 0.0f,
                                                       settings.temporalAa.enabled ? 1.0f : 0.0f};
        postProcess.uniforms.tonemapOptions.x = settings.toneMapping.exposure;
        postProcess.uniforms.tonemapOptions.y = context_.swapchainIsSrgb() ? 1.0f : 0.0f;
        postProcess.uniforms.tonemapOptions.z = std::clamp(settings.temporalAa.sharpness, 0.0f, 1.0f);
        postProcess.uniforms.ambientOcclusionOptions =
            glm::vec4{static_cast<float>(settings.globalIllumination.ambientOcclusionMode),
                      std::max(settings.globalIllumination.ambientOcclusionRadius, 0.05f),
                      std::max(settings.globalIllumination.ambientOcclusionStrength, 0.0f),
                      std::clamp(settings.globalIllumination.ambientOcclusionBias, 0.0f, 0.5f)};

        frameGraph_.reset();
        FrameGraphResourceImporter importer{frameGraph_};
        const nvrhi::ResourceStates frameInitialState = frameResourcesInitialized_[frameIndex]
                                                            ? nvrhi::ResourceStates::ShaderResource
                                                            : nvrhi::ResourceStates::Common;
        const nvrhi::ResourceStates depthInitialState =
            frameResourcesInitialized_[frameIndex] ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::Common;
        const bool hybridPathActive =
            renderPipeline_ != nullptr && activePipelineKind_ == pipelines::DefaultRenderPipelineKind::Hybrid;
        core::RasterSurfaceData rasterSurface;
        core::RtSurfaceData rtSurface;
        RasterPassTargets rasterTargets;
        HybridPassData hybridData;
        hybridData.active = hybridPathActive;
        if (!hybridPathActive) {
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const FrameGraphResourceHandle graphResource =
                    importer.importTexture("shadow.cascade" + std::to_string(cascade),
                                           textureDesc(rasterFrame.shadowCascades[cascade], frameInitialState));
                shadows.cascades[cascade] = textureFrameData(rasterFrame.shadowCascades[cascade], graphResource);
            }
            rasterSurface.position = textureFrameData(
                rasterFrame.position,
                importer.importTexture("gbuffer.position", textureDesc(rasterFrame.position, frameInitialState)));
            rasterSurface.normalRoughness = textureFrameData(
                rasterFrame.normalRoughness,
                importer.importTexture("gbuffer.normal", textureDesc(rasterFrame.normalRoughness, frameInitialState)));
            rasterSurface.albedoMetallic = textureFrameData(
                rasterFrame.albedo,
                importer.importTexture("gbuffer.albedo", textureDesc(rasterFrame.albedo, frameInitialState)));
            rasterSurface.motion = textureFrameData(
                rasterFrame.motion,
                importer.importTexture("gbuffer.motion", textureDesc(rasterFrame.motion, frameInitialState)));
            FrameGraphTextureDesc materialIdDesc = textureDesc(rasterFrame.materialId, frameInitialState);
            materialIdDesc.finalState = nvrhi::ResourceStates::ShaderResource;
            rasterSurface.materialId =
                textureFrameData(rasterFrame.materialId, importer.importTexture("gbuffer.material-id", materialIdDesc));
            if (modelRenderer_ != nullptr) {
                const nvrhi::BufferHandle& materialBuffer = modelRenderer_->materialBuffer(frameIndex);
                rasterSurface.materials = core::BufferFrameData{
                    .buffer = materialBuffer,
                    .graphResource = importer.importBuffer(
                        "gpu-scene.materials",
                        FrameGraphBufferDesc{.size = materialBuffer->getDesc().byteSize,
                                             .buffer = materialBuffer,
                                             .initialState = modelRenderer_->materialBufferInitialState(frameIndex),
                                             .finalState = nvrhi::ResourceStates::ShaderResource}),
                    .readyPass = {},
                };
            }
            FrameGraphTextureDesc depthDesc = textureDesc(rasterFrame.depth, depthInitialState);
            depthDesc.finalState = nvrhi::ResourceStates::DepthWrite;
            rasterSurface.depth =
                textureFrameData(rasterFrame.depth, importer.importTexture("gbuffer.depth", depthDesc));
        }
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        else {
            if (hybridGi_ == nullptr || hybridGi_->directLighting == nullptr) {
                throw std::logic_error("Hybrid render topology requires the RT surface runtime.");
            }
            const gi::RayTracedDiFrameResources& surface = hybridGi_->directLighting->signals(frameIndex);
            const nvrhi::ResourceStates rtSignalInitialState =
                hybridGi_->directLighting->frameSlotInitialized(frameIndex)
                    ? nvrhi::ResourceStates::ShaderResource
                    : nvrhi::ResourceStates::Common;
            const auto importRtSurface = [&importer](std::string name, const nvrhi::TextureHandle& texture,
                                                     nvrhi::ResourceStates initialState) {
                return textureFrameData(
                    texture,
                    importer.importTexture(std::move(name), textureDesc(GpuTexture{texture}, initialState)));
            };
            rtSurface.worldPositionHitDistance =
                importRtSurface("rt.surface.world-position", surface.worldPositionHitT, frameInitialState);
            rtSurface.normalRoughness =
                importRtSurface("rt.surface.normal-roughness", surface.normalRoughness, frameInitialState);
            rtSurface.albedoMetallic =
                importRtSurface("rt.surface.albedo-metallic", surface.albedoMetallic, frameInitialState);
            rtSurface.materialId = importRtSurface("rt.surface.material-id", surface.materialId, frameInitialState);
            rtSurface.motion = importRtSurface("rt.surface.motion", surface.motion, frameInitialState);
            rtSurface.viewDepth = importRtSurface("rt.surface.view-z", surface.viewZ, rtSignalInitialState);
            rtSurface.visibility =
                importRtSurface("rt.surface.visibility", surface.visibilityMask, rtSignalInitialState);
            rtSurface.directDiffuseRadianceHitDistance =
                importRtSurface("rt.surface.direct-diffuse", surface.directDiffuseRadianceHitT,
                                rtSignalInitialState);
            rtSurface.directSpecularRadianceHitDistance =
                importRtSurface("rt.surface.direct-specular", surface.directSpecularRadianceHitT,
                                rtSignalInitialState);
            hybridData.surface.worldPositionHitT = rtSurface.worldPositionHitDistance.graphResource;
            hybridData.surface.normalRoughness = rtSurface.normalRoughness.graphResource;
            hybridData.surface.albedoMetallic = rtSurface.albedoMetallic.graphResource;
            hybridData.surface.materialId = rtSurface.materialId.graphResource;
            hybridData.surface.viewZ = rtSurface.viewDepth.graphResource;
            hybridData.surface.motion = rtSurface.motion.graphResource;
            hybridData.surface.directDiffuseRadianceHitT = rtSurface.directDiffuseRadianceHitDistance.graphResource;
            hybridData.surface.directSpecularRadianceHitT = rtSurface.directSpecularRadianceHitDistance.graphResource;
            hybridData.surface.visibilityMask = rtSurface.visibility.graphResource;
        }
#endif
        core::IndirectLightingData indirectLighting;
        indirectLighting.combined =
            textureFrameData(postFxFrame.globalIllumination,
                             importer.importTexture("global-illumination.output",
                                                    textureDesc(postFxFrame.globalIllumination,
                                                                globalIlluminationInitialized_[frameIndex]
                                                                    ? nvrhi::ResourceStates::ShaderResource
                                                                    : nvrhi::ResourceStates::Common)));
        core::DenoisedLightingData denoisedLighting{
            .direct = {},
            .diffuse = {},
            .specular = {},
            .combined = indirectLighting.combined,
        };
        core::SceneHdrData sceneHdr;
        sceneHdr.color = textureFrameData(
            postFxFrame.lighting,
            importer.importTexture("lighting.hdr", textureDesc(postFxFrame.lighting, frameInitialState)));
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridPathActive) {
            rtSurface.directRadiance =
                textureFrameData(postFxFrame.directRadiance,
                                 importer.importTexture("rt.surface.direct-radiance",
                                                        textureDesc(postFxFrame.directRadiance,
                                                                    directRadianceInitialized_[frameIndex]
                                                                        ? nvrhi::ResourceStates::ShaderResource
                                                                        : nvrhi::ResourceStates::Common)));
            hybridData.surface.directRadiance = rtSurface.directRadiance.graphResource;
            denoisedLighting.direct = textureFrameData(
                postFxFrame.denoisedDirectRadiance,
                importer.importTexture("rt.direct-radiance.denoised",
                                       textureDesc(postFxFrame.denoisedDirectRadiance,
                                                   directNrdOutputInitialized_[frameIndex]
                                                       ? nvrhi::ResourceStates::ShaderResource
                                                       : nvrhi::ResourceStates::Common)));
        }
#endif
        sceneHdr.position = hybridPathActive ? rtSurface.worldPositionHitDistance : rasterSurface.position;
        sceneHdr.motion = hybridPathActive ? rtSurface.motion : rasterSurface.motion;
        sceneHdr.depth = hybridPathActive ? rtSurface.viewDepth : rasterSurface.depth;
        core::TemporalOutputData temporalOutput;
        temporalOutput.color = textureFrameData(
            postFxFrame.taaResolved,
            importer.importTexture("taa.resolved", textureDesc(postFxFrame.taaResolved, frameInitialState)));
        temporalOutput.historyRead = textureFrameData(
            historyReadFrame.history,
            importer.importTexture(
                "taa.history.read",
                textureDesc(historyReadFrame.history, postFxResources_.historyInitialState(historyReadIndex))));
        temporalOutput.historyWrite = textureFrameData(
            postFxFrame.history,
            importer.importTexture("taa.history.write",
                                   textureDesc(postFxFrame.history, postFxResources_.historyInitialState(frameIndex))));

        FrameGraphTextureDesc viewportDesc =
            textureDesc(viewportOutput_, viewportOutputInitialized_ ? nvrhi::ResourceStates::ShaderResource
                                                                    : nvrhi::ResourceStates::Common);
        viewportDesc.finalState = nvrhi::ResourceStates::ShaderResource;
        core::ViewportOutputData viewportOutput{
            .color = textureFrameData(viewportOutput_, importer.importTexture("viewport.output", viewportDesc))};

        FrameGraphTextureDesc swapDesc;
        swapDesc.texture = context_.swapchainTextures().at(imageIndex);
        swapDesc.initialState = context_.swapchainTextureInitialState(imageIndex);
        if (swapDesc.initialState == nvrhi::ResourceStates::Unknown) {
            swapDesc.initialState = nvrhi::ResourceStates::Common;
        }
        swapDesc.finalState = nvrhi::ResourceStates::Present;
        core::PresentationInputData presentationInput;
        if (currentUiDrawData_ == nullptr) {
            throw std::logic_error("Default pipeline session requires current ImGui draw data.");
        }
        presentationInput.ui = currentUiDrawData_;
        presentationInput.imageIndex = imageIndex;
        presentationInput.frameSlot = frameIndex;
        presentationInput.swapchain =
            textureFrameData(swapDesc.texture, importer.importTexture("swapchain.color", swapDesc));
        FrameGraphTextureDesc fontDesc;
        fontDesc.texture = presentation_.fontTexture();
        fontDesc.initialState = presentation_.fontTextureInitialState();
        fontDesc.finalState = nvrhi::ResourceStates::ShaderResource;
        presentationInput.fontAtlas =
            textureFrameData(fontDesc.texture, importer.importTexture("imgui.font", fontDesc));

        nvrhi::IDevice& device = *context_.rhiDevice();
        if (!hybridPathActive) {
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                rasterTargets.shadowFramebuffers[cascade] = createFramebuffer(
                    device, nvrhi::FramebufferDesc().setDepthAttachment(rasterFrame.shadowCascades[cascade].texture));
            }
            nvrhi::FramebufferDesc gbufferDesc;
            gbufferDesc.addColorAttachment(rasterFrame.position.texture)
                .addColorAttachment(rasterFrame.normalRoughness.texture)
                .addColorAttachment(rasterFrame.albedo.texture)
                .addColorAttachment(rasterFrame.motion.texture)
                .addColorAttachment(rasterFrame.materialId.texture)
                .setDepthAttachment(rasterFrame.depth.texture);
            rasterTargets.surfaceFramebuffer = createFramebuffer(device, gbufferDesc);
            postProcess.lightingFramebuffer =
                createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(postFxFrame.lighting.texture));
        }
        postProcess.temporalFramebuffer =
            createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(postFxFrame.taaResolved.texture));
        postProcess.toneMappingFramebuffer =
            createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(viewportOutput_.texture));

        core::RenderBlackboard blackboard;
        blackboard.set(std::move(sceneData));
        blackboard.set(std::move(shadows));
        if (hybridPathActive) {
            blackboard.set(std::move(rtSurface));
        } else {
            blackboard.set(std::move(rasterSurface));
        }
        blackboard.set(std::move(indirectLighting));
        blackboard.set(std::move(denoisedLighting));
        blackboard.set(std::move(sceneHdr));
        blackboard.set(std::move(temporalOutput));
        blackboard.set(std::move(viewportOutput));
        blackboard.set(std::move(presentationInput));
        blackboard.set(std::move(rasterTargets));
        blackboard.set(std::move(postProcess));
        blackboard.set(AtmospherePassData{});
        blackboard.set(std::move(hybridData));
        blackboard.set(FrameImportServices{.importer = &importer});
        renderPipeline_->prepareFrame(identity, camera.cutEpoch, changes, frameGraph_, blackboard);
        frameGraph_.execute(FrameGraphContext{&commandList, nullptr, frameIndex});
        const HybridPassData& completedHybrid = blackboard.get<HybridPassData>();
        const bool usedHybridPath = completedHybrid.active;
        const core::DenoisedLightingData& completedLighting = blackboard.get<core::DenoisedLightingData>();
        const bool usedDirectNrd =
            usedHybridPath && completedLighting.direct.texture == postFxFrame.denoisedDirectRadiance.texture;
        return RecordedFrameState{
            .viewProjection = viewProjection,
            .view = view,
            .projection = unjitteredProjection,
            .jitter = jitter,
            .featureConfiguration = featureConfiguration(settings),
            .usedHybridPath = usedHybridPath,
            .usedDirectNrd = usedDirectNrd,
            .usedIndirectLighting = usedHybridPath && completedHybrid.globalIlluminationActive,
        };
    }

} // namespace lumin::render

#include "lumin/render/LevelRenderer.hpp"

#include "lumin/platform/Window.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/render/gi/SsaoBackend.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/trigonometric.hpp>

namespace lumin::render {
    namespace {

        constexpr float cameraNearPlane = 0.05f;
        constexpr float cameraFarPlane = 200.0f;
        constexpr float cascadeSplitLambda = 0.68f;

        struct CascadeShadowData {
            std::array<glm::mat4, shadowCascadeCount> viewProjections{};
            glm::vec4 splits{0.0f};
        };

        glm::vec3 normalizedLightDirection(glm::vec3 direction) {
            if (glm::dot(direction, direction) < 1e-6f) {
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

        CascadeShadowData calculateCascadeShadows(const scene::Camera& camera, float aspectRatio,
                                                  glm::vec3 lightDirection) {
            CascadeShadowData result;
            std::array<float, shadowCascadeCount> splits{};
            const float clipRange = cameraFarPlane - cameraNearPlane;
            const float clipRatio = cameraFarPlane / cameraNearPlane;
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(shadowCascadeCount);
                const float logarithmic = cameraNearPlane * std::pow(clipRatio, fraction);
                const float uniform = cameraNearPlane + clipRange * fraction;
                splits[cascade] = cascadeSplitLambda * logarithmic + (1.0f - cascadeSplitLambda) * uniform;
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

            float sliceNear = cameraNearPlane;
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
                glm::mat4 lightProjection =
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

    LevelRenderer::LevelRenderer(platform::Window& window, VulkanContext& context, const scene::Level& level,
                                 std::filesystem::path shaderDirectory,
                                 std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination)
        : window_(window), context_(context), level_(level), shaderDirectory_(std::move(shaderDirectory)),
          textures_(context), pipelines_(context, shaderDirectory_),
          globalIllumination_(std::move(globalIllumination)) {
        if (globalIllumination_ == nullptr) {
            globalIllumination_ = gi::makeSsaoBackend(shaderDirectory_);
        }
        createRenderResources();
        imgui_.initialize(window_, context_);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    LevelRenderer::~LevelRenderer() {
        waitIdle();
        imgui_.shutdown();
        destroyRenderResources();
    }

    void LevelRenderer::beginUiFrame(ImGuiContent* content) {
        if (swapchainGeneration_ != context_.swapchainGeneration()) {
            refreshSwapchainResources();
        }
        imgui_.beginFrame(content);
    }

    void LevelRenderer::cancelUiFrame() noexcept {
        imgui_.cancelFrame();
    }

    void LevelRenderer::drawFrame(scene::Camera& camera, RenderSettings& settings, ImGuiContent* content) {
        if (swapchainGeneration_ != context_.swapchainGeneration()) {
            refreshSwapchainResources();
        }
        if (topologyRevision_ != level_.topologyRevision()) {
            context_.waitIdle();
            globalIllumination_->invalidateHistory();
            modelRenderer_.reset();
            createModelRenderer();
            textures_.invalidateHistory();
            hasPreviousCamera_ = false;
        }

        if (!imgui_.framePrepared()) {
            imgui_.beginFrame(content);
        }

        const std::optional<VulkanFrame> frame = context_.beginFrame();
        if (!frame.has_value()) {
            imgui_.cancelFrame();
            refreshSwapchainResources();
            return;
        }

        RecordedFrameState recorded;
        try {
            recorded = recordCommandList(*frame->commandList, frame->frameIndex, frame->imageIndex, camera, settings);
        } catch (...) {
            imgui_.cancelFrame();
            context_.cancelFrame(*frame);
            refreshSwapchainResources();
            throw;
        }

        const bool recreate = context_.submitFrame(*frame);
        textures_.markHistoryValid(frame->frameIndex);
        frameResourcesInitialized_[frame->frameIndex] = true;
        previousViewProjection_ = recorded.viewProjection;
        previousCameraPosition_ = recorded.cameraPosition;
        previousCameraForward_ = recorded.cameraForward;
        previousFieldOfView_ = recorded.fieldOfView;
        previousTaaEnabled_ = recorded.taaEnabled;
        previousGlobalIlluminationEnabled_ = recorded.globalIlluminationEnabled;
        hasPreviousCamera_ = true;
        ++frameNumber_;
        if (recreate) {
            refreshSwapchainResources();
        }
    }

    void LevelRenderer::waitIdle() const {
        context_.waitIdle();
    }

    ImGuiCaptureState LevelRenderer::imguiCaptureState() const noexcept {
        return imgui_.captureState();
    }

    std::uint32_t LevelRenderer::modelCount() const noexcept {
        return modelRenderer_ == nullptr ? 0 : modelRenderer_->drawCount();
    }

    std::uint32_t LevelRenderer::mdiDrawCount() const noexcept {
        return modelCount();
    }

    gi::BackendInfo LevelRenderer::globalIlluminationBackendInfo() const noexcept {
        return globalIllumination_->info();
    }

    void LevelRenderer::createRenderResources() {
        const std::uint32_t width = context_.swapchainWidth();
        const std::uint32_t height = context_.swapchainHeight();
        textures_.create(width, height);
        pipelines_.create(textures_.bindingLayout(), textures_.lightingFormat(), context_.swapchainRhiFormat());

        std::array<gi::FrameResources, TextureManager::maxFramesInFlight> giFrames{};
        for (std::uint32_t frameIndex = 0; frameIndex < giFrames.size(); ++frameIndex) {
            const TextureFrameResources& frame = textures_.frame(frameIndex);
            giFrames[frameIndex].position = frame.position.texture;
            giFrames[frameIndex].normalRoughness = frame.normalRoughness.texture;
            giFrames[frameIndex].albedoMetallic = frame.albedo.texture;
            giFrames[frameIndex].motion = frame.motion.texture;
            giFrames[frameIndex].depth = frame.depth.texture;
            giFrames[frameIndex].uniformBuffer = frame.postProcessUniform.buffer;
            giFrames[frameIndex].output = frame.globalIllumination.texture;
        }
        globalIllumination_->create(gi::CreateInfo{context_.rhiDevice(),
                                                   {width, height},
                                                   textures_.globalIlluminationFormat(),
                                                   textures_.sampler(),
                                                   giFrames});
        createModelRenderer();
        hasPreviousCamera_ = false;
        previousGlobalIlluminationEnabled_ = true;
        frameNumber_ = 0;
        frameResourcesInitialized_.fill(false);
    }

    void LevelRenderer::createModelRenderer() {
        topologyRevision_ = level_.topologyRevision();
        if (level_.models().empty()) {
            modelRenderer_.reset();
            return;
        }
        const std::array<nvrhi::Format, 4> colorFormats = {textures_.positionFormat(), textures_.normalFormat(),
                                                           textures_.albedoFormat(), textures_.motionFormat()};
        modelRenderer_ = std::make_unique<ModelRenderer>(
            context_, level_, shaderDirectory_, colorFormats, textures_.depthFormat(), textures_.shadowDepthFormat(),
            TextureManager::maxFramesInFlight, context_.modelRendererCapabilities());
    }

    void LevelRenderer::destroyRenderResources() noexcept {
        modelRenderer_.reset();
        globalIllumination_->destroy();
        pipelines_.destroy();
        textures_.destroy();
    }

    void LevelRenderer::refreshSwapchainResources() {
        context_.waitIdle();
        globalIllumination_->invalidateHistory();
        imgui_.shutdown();
        destroyRenderResources();
        createRenderResources();
        imgui_.initialize(window_, context_);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    LevelRenderer::RecordedFrameState LevelRenderer::recordCommandList(nvrhi::ICommandList& commandList,
                                                                       std::uint32_t frameIndex,
                                                                       std::uint32_t imageIndex,
                                                                       const scene::Camera& camera,
                                                                       const RenderSettings& settings) {
        const std::uint32_t width = context_.swapchainWidth();
        const std::uint32_t height = context_.swapchainHeight();
        if (width == 0 || height == 0) {
            throw std::runtime_error("LevelRenderer cannot record a frame with a zero render extent.");
        }
        const float aspectRatio = static_cast<float>(width) / static_cast<float>(std::max(1U, height));
        const glm::mat4 view = camera.viewMatrix();
        const glm::mat4 unjitteredViewProjection = camera.projectionMatrix(aspectRatio) * view;
        const glm::vec3 cameraForward = camera.forward();
        const bool cameraCut = !hasPreviousCamera_ || glm::length(camera.position() - previousCameraPosition_) > 5.0f ||
                               glm::dot(cameraForward, previousCameraForward_) < std::cos(glm::radians(20.0f)) ||
                               std::abs(camera.fieldOfViewDegrees() - previousFieldOfView_) > 1.0f ||
                               (!previousTaaEnabled_ && settings.enableTaa);
        const bool globalIlluminationReenabled =
            !previousGlobalIlluminationEnabled_ && settings.enableGlobalIllumination;
        if (gi::shouldInvalidateHistory(gi::HistoryInvalidationState{
                .cameraCut = cameraCut, .backendReenabled = globalIlluminationReenabled})) {
            globalIllumination_->invalidateHistory();
        }
        if (cameraCut) {
            textures_.invalidateHistory();
            frameNumber_ = 0;
        }
        glm::mat4 projection = camera.projectionMatrix(aspectRatio);
        if (settings.enableTaa) {
            const std::uint32_t jitterIndex = static_cast<std::uint32_t>(frameNumber_ % 8U) + 1U;
            const glm::vec2 jitter{halton(jitterIndex, 2U) - 0.5f, halton(jitterIndex, 3U) - 0.5f};
            projection[2][0] += (2.0f * jitter.x) / static_cast<float>(width);
            projection[2][1] += (2.0f * jitter.y) / static_cast<float>(height);
        }
        const glm::mat4 viewProjection = projection * view;
        const glm::mat4 previousViewProjection = cameraCut ? viewProjection : previousViewProjection_;
        if (modelRenderer_ != nullptr) {
            modelRenderer_->sync(level_, frameIndex, cameraCut);
        }

        const glm::vec3 lightDirection = normalizedLightDirection(settings.sunDirection);
        const CascadeShadowData cascades = calculateCascadeShadows(camera, aspectRatio, lightDirection);
        const std::uint32_t historyReadIndex =
            (frameIndex + TextureManager::maxFramesInFlight - 1) % TextureManager::maxFramesInFlight;
        PostProcessUniforms uniforms;
        uniforms.inverseViewProjection = glm::inverse(unjitteredViewProjection);
        uniforms.viewProjection = viewProjection;
        uniforms.cascadeViewProjections = cascades.viewProjections;
        uniforms.cascadeSplits = cascades.splits;
        uniforms.cameraPosition = glm::vec4{camera.position(), 1.0f};
        uniforms.cameraForward = glm::vec4{cameraForward, 0.0f};
        uniforms.lightDirection = glm::vec4{lightDirection, 0.0f};
        uniforms.renderSize = glm::vec4{static_cast<float>(width), static_cast<float>(height),
                                        1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
        uniforms.renderOptions = glm::vec4{textures_.historyValid(historyReadIndex) ? 1.0f : 0.0f,
                                           settings.enableGlobalIllumination ? 1.0f : 0.0f,
                                           settings.enableShadows ? 1.0f : 0.0f, settings.enableTaa ? 1.0f : 0.0f};
        uniforms.tonemapOptions.x = settings.exposure;
        uniforms.tonemapOptions.y = context_.swapchainIsSrgb() ? 1.0f : 0.0f;
        textures_.updatePostProcessUniforms(frameIndex, uniforms);

        const TextureFrameResources& frame = textures_.frame(frameIndex);
        const TextureFrameResources& historyReadFrame = textures_.frame(historyReadIndex);
        frameGraph_.reset();

        std::array<FrameGraphResourceHandle, shadowCascadeCount> shadows{};
        const nvrhi::ResourceStates frameInitialState = frameResourcesInitialized_[frameIndex]
                                                            ? nvrhi::ResourceStates::ShaderResource
                                                            : nvrhi::ResourceStates::Common;
        const nvrhi::ResourceStates depthInitialState = frameResourcesInitialized_[frameIndex]
                                                            ? nvrhi::ResourceStates::DepthWrite
                                                            : nvrhi::ResourceStates::Common;
        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            shadows[cascade] = frameGraph_.importTexture("shadow.cascade" + std::to_string(cascade),
                                                         textureDesc(frame.shadowCascades[cascade], frameInitialState));
        }
        const auto position =
            frameGraph_.importTexture("gbuffer.position", textureDesc(frame.position, frameInitialState));
        const auto normal =
            frameGraph_.importTexture("gbuffer.normal", textureDesc(frame.normalRoughness, frameInitialState));
        const auto albedo = frameGraph_.importTexture("gbuffer.albedo", textureDesc(frame.albedo, frameInitialState));
        const auto motion = frameGraph_.importTexture("gbuffer.motion", textureDesc(frame.motion, frameInitialState));
        const auto depth = frameGraph_.importTexture("gbuffer.depth", textureDesc(frame.depth, depthInitialState));
        const auto globalIllumination = frameGraph_.importTexture(
            "global-illumination.output", textureDesc(frame.globalIllumination, frameInitialState));
        const auto lighting = frameGraph_.importTexture("lighting.hdr", textureDesc(frame.lighting, frameInitialState));
        const auto taaResolved =
            frameGraph_.importTexture("taa.resolved", textureDesc(frame.taaResolved, frameInitialState));
        const auto historyRead = frameGraph_.importTexture(
            "taa.history.read", textureDesc(historyReadFrame.history, textures_.historyInitialState(historyReadIndex)));
        const auto historyWrite = frameGraph_.importTexture(
            "taa.history.write", textureDesc(frame.history, textures_.historyInitialState(frameIndex)));

        FrameGraphTextureDesc swapDesc;
        swapDesc.texture = context_.swapchainTextures().at(imageIndex);
        swapDesc.initialState = context_.swapchainTextureInitialState(imageIndex);
        if (swapDesc.initialState == nvrhi::ResourceStates::Unknown) {
            swapDesc.initialState = nvrhi::ResourceStates::Common;
        }
        swapDesc.finalState = nvrhi::ResourceStates::Present;
        const auto swap = frameGraph_.importTexture("swapchain.color", swapDesc);
        FrameGraphTextureDesc fontDesc;
        fontDesc.texture = imgui_.fontTexture();
        fontDesc.initialState = imgui_.fontTextureInitialState();
        fontDesc.finalState = nvrhi::ResourceStates::ShaderResource;
        const auto imguiFont = frameGraph_.importTexture("imgui.font", fontDesc);

        nvrhi::IDevice& device = *context_.rhiDevice();
        std::array<nvrhi::FramebufferHandle, shadowCascadeCount> shadowFramebuffers{};
        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            shadowFramebuffers[cascade] = createFramebuffer(
                device, nvrhi::FramebufferDesc().setDepthAttachment(frame.shadowCascades[cascade].texture));
        }
        nvrhi::FramebufferDesc gbufferDesc;
        gbufferDesc.addColorAttachment(frame.position.texture)
            .addColorAttachment(frame.normalRoughness.texture)
            .addColorAttachment(frame.albedo.texture)
            .addColorAttachment(frame.motion.texture)
            .setDepthAttachment(frame.depth.texture);
        const nvrhi::FramebufferHandle gbufferFramebuffer = createFramebuffer(device, gbufferDesc);
        const nvrhi::FramebufferHandle lightingFramebuffer =
            createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(frame.lighting.texture));
        const nvrhi::FramebufferHandle taaFramebuffer =
            createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(frame.taaResolved.texture));
        const nvrhi::FramebufferHandle tonemapFramebuffer = createFramebuffer(
            device, nvrhi::FramebufferDesc().addColorAttachment(context_.swapchainTextures().at(imageIndex)));

        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            frameGraph_.addPass(
                "CSM clear " + std::to_string(cascade), FrameGraphPassType::Transfer,
                [shadow = shadows[cascade]](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::CopyDest);
                },
                [texture = frame.shadowCascades[cascade].texture](const FrameGraphContext& context) {
                    context.commandList->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, 1.0f, false,
                                                                  0);
                });
            frameGraph_.addPass(
                "CSM cascade " + std::to_string(cascade), FrameGraphPassType::Graphics,
                [shadow = shadows[cascade]](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::DepthWrite);
                },
                [this, frameIndex, cascade, &cascades,
                 framebuffer = shadowFramebuffers[cascade]](const FrameGraphContext& context) {
                    recordShadowPass(*context.commandList, *framebuffer, frameIndex, cascade,
                                     cascades.viewProjections[cascade]);
                });
        }

        frameGraph_.addPass(
            "G-buffer clear", FrameGraphPassType::Transfer,
            [position, normal, albedo, motion, depth](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color : {position, normal, albedo, motion}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::CopyDest);
                }
                builder.writeTexture(depth, nvrhi::ResourceStates::CopyDest);
            },
            [&frame](const FrameGraphContext& context) {
                const std::array<nvrhi::Color, 4> colors = {
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}, nvrhi::Color{0.0f, 0.0f, 1.0f, 1.0f},
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f}, nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}};
                const std::array<const GpuTexture*, 4> images = {&frame.position, &frame.normalRoughness,
                                                                 &frame.albedo, &frame.motion};
                for (std::uint32_t index = 0; index < images.size(); ++index) {
                    context.commandList->clearTextureFloat(images[index]->texture, nvrhi::AllSubresources,
                                                           colors[index]);
                }
                context.commandList->clearDepthStencilTexture(frame.depth.texture, nvrhi::AllSubresources, true,
                                                              1.0f, false, 0);
            });
        frameGraph_.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [position, normal, albedo, motion, depth](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color : {position, normal, albedo, motion}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::RenderTarget);
                }
                builder.writeTexture(depth, nvrhi::ResourceStates::DepthWrite);
            },
            [this, frameIndex, viewProjection, previousViewProjection,
             framebuffer = gbufferFramebuffer](const FrameGraphContext& context) {
                recordGBufferPass(*context.commandList, *framebuffer, frameIndex, viewProjection,
                                  previousViewProjection);
            });

        globalIllumination_->addPasses(frameGraph_, gi::FrameInfo{level_,
                                                                  frameIndex,
                                                                  frameNumber_,
                                                                  settings.enableGlobalIllumination,
                                                                  cameraCut,
                                                                  {width, height},
                                                                  position,
                                                                  normal,
                                                                  albedo,
                                                                  motion,
                                                                  depth,
                                                                  globalIllumination});

        frameGraph_.addPass(
            "Procedural sky clear", FrameGraphPassType::Transfer,
            [lighting](FrameGraphBuilder& builder) {
                builder.writeTexture(lighting, nvrhi::ResourceStates::CopyDest);
            },
            [texture = frame.lighting.texture](const FrameGraphContext& context) {
                context.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                       nvrhi::Color{0.035f, 0.04f, 0.05f, 1.0f});
            });
        frameGraph_.addPass(
            "Procedural sky", FrameGraphPassType::Graphics,
            [globalIllumination, lighting](FrameGraphBuilder& builder) {
                builder.readTexture(globalIllumination, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(lighting, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex, framebuffer = lightingFramebuffer](const FrameGraphContext& context) {
                recordFullscreenPass(*context.commandList, *framebuffer, pipelines_.sky(), frameIndex);
            });

        frameGraph_.addPass(
            "Deferred lighting", FrameGraphPassType::Graphics,
            [position, normal, albedo, globalIllumination, shadows, lighting](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input : {position, normal, albedo, globalIllumination}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                for (FrameGraphResourceHandle shadow : shadows) {
                    builder.readTexture(shadow, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(lighting, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex, framebuffer = lightingFramebuffer](const FrameGraphContext& context) {
                recordFullscreenPass(*context.commandList, *framebuffer, pipelines_.deferredLighting(), frameIndex);
            });

        frameGraph_.addPass(
            "TAA clear", FrameGraphPassType::Transfer,
            [taaResolved](FrameGraphBuilder& builder) {
                builder.writeTexture(taaResolved, nvrhi::ResourceStates::CopyDest);
            },
            [texture = frame.taaResolved.texture](const FrameGraphContext& context) {
                context.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                       nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f});
            });
        frameGraph_.addPass(
            "TAA resolve", FrameGraphPassType::Graphics,
            [lighting, motion, historyRead, taaResolved](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input : {lighting, motion, historyRead}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(taaResolved, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex, framebuffer = taaFramebuffer](const FrameGraphContext& context) {
                recordFullscreenPass(*context.commandList, *framebuffer, pipelines_.taa(), frameIndex);
            });

        frameGraph_.addPass(
            "TAA history copy", FrameGraphPassType::Transfer,
            [taaResolved, historyWrite](FrameGraphBuilder& builder) {
                builder.readTexture(taaResolved, nvrhi::ResourceStates::CopySource);
                builder.writeTexture(historyWrite, nvrhi::ResourceStates::CopyDest);
            },
            [this, frameIndex](const FrameGraphContext& context) {
                recordHistoryCopy(*context.commandList, frameIndex);
            });

        frameGraph_.addPass(
            "TAA history ready", FrameGraphPassType::Transfer,
            [historyWrite](FrameGraphBuilder& builder) {
                builder.readTexture(historyWrite, nvrhi::ResourceStates::ShaderResource);
            },
            nullptr);

        frameGraph_.addPass(
            "Tonemap", FrameGraphPassType::Graphics,
            [taaResolved, historyWrite, swap](FrameGraphBuilder& builder) {
                builder.readTexture(taaResolved, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(historyWrite, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(swap, nvrhi::ResourceStates::RenderTarget);
            },
            [this, frameIndex, framebuffer = tonemapFramebuffer](const FrameGraphContext& context) {
                recordFullscreenPass(*context.commandList, *framebuffer, pipelines_.tonemap(), frameIndex);
            });

        frameGraph_.addPass(
            "ImGui overlay", FrameGraphPassType::Graphics,
            [swap, imguiFont](FrameGraphBuilder& builder) {
                builder.readTexture(imguiFont, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(swap, nvrhi::ResourceStates::RenderTarget);
            },
            [this, imageIndex, frameIndex](const FrameGraphContext& context) {
                imgui_.record(*context.commandList, imageIndex, frameIndex);
            });

        frameGraph_.addPass(
            "Present", FrameGraphPassType::Present,
            [swap](FrameGraphBuilder& builder) {
                builder.readTexture(swap, nvrhi::ResourceStates::Present);
            },
            nullptr);

        frameGraph_.execute(FrameGraphContext{&commandList, nullptr, frameIndex});
        imgui_.markFontTextureInitialized();
        return RecordedFrameState{viewProjection,     camera.position(),
                                  cameraForward,      camera.fieldOfViewDegrees(),
                                  settings.enableTaa, settings.enableGlobalIllumination};
    }

    void LevelRenderer::recordShadowPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                         std::uint32_t frameIndex, std::uint32_t cascadeIndex,
                                         const glm::mat4& lightViewProjection) {
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordShadow(commandList, framebuffer, shadowMapResolution, shadowMapResolution,
                                         frameIndex, cascadeIndex, lightViewProjection);
        }
    }

    void LevelRenderer::recordGBufferPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                          std::uint32_t frameIndex, const glm::mat4& viewProjection,
                                          const glm::mat4& previousViewProjection) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordGBuffer(commandList, framebuffer, frame.position.width, frame.position.height,
                                          frameIndex, viewProjection, previousViewProjection);
        }
    }

    void LevelRenderer::recordFullscreenPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                             const nvrhi::GraphicsPipelineHandle& pipeline,
                                             std::uint32_t frameIndex) {
        const std::uint32_t width = context_.swapchainWidth();
        const std::uint32_t height = context_.swapchainHeight();
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
            .setFramebuffer(&framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
            .addBindingSet(textures_.bindingSet(frameIndex));
        commandList.setGraphicsState(state);
        commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
    }

    void LevelRenderer::recordHistoryCopy(nvrhi::ICommandList& commandList, std::uint32_t frameIndex) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        commandList.copyTexture(frame.history.texture, nvrhi::TextureSlice{}, frame.taaResolved.texture,
                                nvrhi::TextureSlice{});
    }

} // namespace lumin::render

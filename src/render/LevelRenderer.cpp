#include "lumin/render/LevelRenderer.hpp"

#include "lumin/platform/Window.hpp"
#include "lumin/render/VulkanContext.hpp"
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

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

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

        bool isSrgbFormat(VkFormat format) {
            return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB ||
                   format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
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
                lightProjection[1][1] *= -1.0f;
                result.viewProjections[cascade] = lightProjection * lightView;
                sliceNear = sliceFar;
            }
            return result;
        }

        FrameGraphTextureDesc textureDesc(const VulkanImage& image, VkExtent2D extent, VkImageUsageFlags usage,
                                          VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED) {
            FrameGraphTextureDesc desc;
            desc.width = extent.width;
            desc.height = extent.height;
            desc.format = image.format;
            desc.usage = usage;
            desc.image = image.image;
            desc.aspectMask = image.aspectMask;
            desc.initialLayout = initialLayout;
            if (initialLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                desc.initialStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                desc.initialAccess = VK_ACCESS_SHADER_READ_BIT;
            }
            return desc;
        }

        void setViewportAndScissor(VkCommandBuffer commandBuffer, VkExtent2D extent) {
            const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                      0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        }

        void recordFullscreen(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D extent,
                              const GraphicsPipeline& pipeline, VkDescriptorSet descriptorSet,
                              VkAttachmentLoadOp loadOp, VkClearColorValue clearColor = {}) {
            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = targetView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = loadOp;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = clearColor;
            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea.extent = extent;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(commandBuffer, &renderingInfo);
            setViewportAndScissor(commandBuffer, extent);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
                                    &descriptorSet, 0, nullptr);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(commandBuffer);
        }

    } // namespace

    LevelRenderer::LevelRenderer(platform::Window& window, VulkanContext& context, const scene::Level& level,
                                 std::filesystem::path shaderDirectory)
        : window_(window), context_(context), level_(level), shaderDirectory_(std::move(shaderDirectory)),
          textures_(context), pipelines_(context, shaderDirectory_) {
        createRenderResources();
        imgui_.initialize(window_, context_);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    LevelRenderer::~LevelRenderer() {
        waitIdle();
        imgui_.shutdown();
        destroyRenderResources();
    }

    void LevelRenderer::drawFrame(scene::Camera& camera, RenderSettings& settings) {
        if (swapchainGeneration_ != context_.swapchainGeneration()) {
            refreshSwapchainResources();
        }
        if (topologyRevision_ != level_.topologyRevision()) {
            context_.waitIdle();
            modelRenderer_.reset();
            createModelRenderer();
            textures_.invalidateHistory();
            hasPreviousCamera_ = false;
        }

        const std::optional<VulkanFrame> frame = context_.beginFrame();
        if (!frame.has_value()) {
            refreshSwapchainResources();
            return;
        }

        imgui_.beginFrame();
        imgui_.drawLevelPanel(camera, settings, modelCount(), mdiDrawCount());
        recordCommandBuffer(frame->commandBuffer, frame->frameIndex, frame->imageIndex, camera, settings);
        if (context_.submitFrame(*frame)) {
            refreshSwapchainResources();
        }
    }

    void LevelRenderer::waitIdle() const {
        context_.waitIdle();
    }

    std::uint32_t LevelRenderer::modelCount() const noexcept {
        return modelRenderer_ == nullptr ? 0 : modelRenderer_->drawCount();
    }

    std::uint32_t LevelRenderer::mdiDrawCount() const noexcept {
        return modelCount();
    }

    void LevelRenderer::createRenderResources() {
        const VkExtent2D extent = context_.swapchainExtent();
        textures_.create(extent);
        pipelines_.create(textures_.descriptorSetLayout(), textures_.ambientOcclusionFormat(),
                          textures_.lightingFormat(), context_.swapchainFormat());
        createModelRenderer();
        hasPreviousCamera_ = false;
        frameNumber_ = 0;
    }

    void LevelRenderer::createModelRenderer() {
        topologyRevision_ = level_.topologyRevision();
        if (level_.models().empty()) {
            modelRenderer_.reset();
            return;
        }
        const std::array<VkFormat, 4> colorFormats = {textures_.positionFormat(), textures_.normalFormat(),
                                                      textures_.albedoFormat(), textures_.motionFormat()};
        modelRenderer_ =
            std::make_unique<ModelRenderer>(context_, level_, shaderDirectory_, colorFormats, textures_.depthFormat(),
                                            textures_.shadowDepthFormat(), TextureManager::maxFramesInFlight);
    }

    void LevelRenderer::destroyRenderResources() noexcept {
        modelRenderer_.reset();
        pipelines_.destroy();
        textures_.destroy();
    }

    void LevelRenderer::refreshSwapchainResources() {
        context_.waitIdle();
        imgui_.shutdown();
        destroyRenderResources();
        createRenderResources();
        imgui_.initialize(window_, context_);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    void LevelRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                            std::uint32_t imageIndex, const scene::Camera& camera,
                                            const RenderSettings& settings) {
        const VkExtent2D extent = context_.swapchainExtent();
        const float aspectRatio = static_cast<float>(extent.width) / static_cast<float>(std::max(1U, extent.height));
        const glm::mat4 view = camera.viewMatrix();
        const glm::mat4 unjitteredViewProjection = camera.projectionMatrix(aspectRatio) * view;
        const glm::vec3 cameraForward = camera.forward();
        const bool cameraCut = !hasPreviousCamera_ || glm::length(camera.position() - previousCameraPosition_) > 5.0f ||
                               glm::dot(cameraForward, previousCameraForward_) < std::cos(glm::radians(20.0f)) ||
                               std::abs(camera.fieldOfViewDegrees() - previousFieldOfView_) > 1.0f ||
                               (!previousTaaEnabled_ && settings.enableTaa);
        if (cameraCut) {
            textures_.invalidateHistory();
            frameNumber_ = 0;
        }
        glm::mat4 projection = camera.projectionMatrix(aspectRatio);
        if (settings.enableTaa) {
            const std::uint32_t jitterIndex = static_cast<std::uint32_t>(frameNumber_ % 8U) + 1U;
            const glm::vec2 jitter{halton(jitterIndex, 2U) - 0.5f, halton(jitterIndex, 3U) - 0.5f};
            projection[2][0] += (2.0f * jitter.x) / static_cast<float>(extent.width);
            projection[2][1] += (2.0f * jitter.y) / static_cast<float>(extent.height);
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
        uniforms.renderSize =
            glm::vec4{static_cast<float>(extent.width), static_cast<float>(extent.height),
                      1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
        uniforms.renderOptions =
            glm::vec4{textures_.historyValid(historyReadIndex) ? 1.0f : 0.0f, settings.enableSsao ? 1.0f : 0.0f,
                      settings.enableShadows ? 1.0f : 0.0f, settings.enableTaa ? 1.0f : 0.0f};
        uniforms.tonemapOptions.x = settings.exposure;
        uniforms.tonemapOptions.y = isSrgbFormat(context_.swapchainFormat()) ? 1.0f : 0.0f;
        textures_.updatePostProcessUniforms(frameIndex, uniforms);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin LevelRenderer command buffer.");

        const TextureFrameResources& frame = textures_.frame(frameIndex);
        const TextureFrameResources& historyReadFrame = textures_.frame(historyReadIndex);
        frameGraph_.reset();

        std::array<FrameGraphResourceHandle, shadowCascadeCount> shadows{};
        const VkExtent2D shadowExtent{shadowMapResolution, shadowMapResolution};
        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            shadows[cascade] = frameGraph_.importTexture(
                "shadow.cascade" + std::to_string(cascade),
                textureDesc(frame.shadowCascades[cascade], shadowExtent,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        }
        const auto position = frameGraph_.importTexture(
            "gbuffer.position",
            textureDesc(frame.position, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto normal = frameGraph_.importTexture(
            "gbuffer.normal", textureDesc(frame.normalRoughness, extent,
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto albedo = frameGraph_.importTexture(
            "gbuffer.albedo",
            textureDesc(frame.albedo, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto motion = frameGraph_.importTexture(
            "gbuffer.motion",
            textureDesc(frame.motion, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto depth = frameGraph_.importTexture(
            "gbuffer.depth", textureDesc(frame.depth, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
        const auto ambientOcclusion = frameGraph_.importTexture(
            "ssao", textureDesc(frame.ambientOcclusion, extent,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto lighting = frameGraph_.importTexture(
            "lighting.hdr",
            textureDesc(frame.lighting, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto taaResolved = frameGraph_.importTexture(
            "taa.resolved", textureDesc(frame.taaResolved, extent,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        const VkImageLayout historyWriteInitial = textures_.historyInitialized(frameIndex)
                                                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImageLayout historyReadInitial = textures_.historyInitialized(historyReadIndex)
                                                     ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_UNDEFINED;
        const auto historyRead = frameGraph_.importTexture(
            "taa.history.read",
            textureDesc(historyReadFrame.history, extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        historyReadInitial));
        const auto historyWrite = frameGraph_.importTexture(
            "taa.history.write",
            textureDesc(frame.history, extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        historyWriteInitial));

        FrameGraphTextureDesc swapDesc;
        swapDesc.width = extent.width;
        swapDesc.height = extent.height;
        swapDesc.format = context_.swapchainFormat();
        swapDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapDesc.image = context_.swapchainImages()[imageIndex];
        swapDesc.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        swapDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        swapDesc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        const auto swap = frameGraph_.importTexture("swapchain.color", swapDesc);

        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            frameGraph_.addPass(
                "CSM cascade " + std::to_string(cascade), FrameGraphPassType::Graphics,
                [shadow = shadows[cascade]](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                },
                [this, commandBuffer, frameIndex, cascade, &cascades](const FrameGraphContext&) {
                    recordShadowPass(commandBuffer, frameIndex, cascade, cascades.viewProjections[cascade]);
                });
        }

        frameGraph_.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [position, normal, albedo, motion, depth](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color : {position, normal, albedo, motion}) {
                    builder.writeTexture(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                }
                builder.writeTexture(depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            },
            [this, commandBuffer, frameIndex, viewProjection, previousViewProjection](const FrameGraphContext&) {
                recordGBufferPass(commandBuffer, frameIndex, viewProjection, previousViewProjection);
            });

        frameGraph_.addPass(
            "SSAO", FrameGraphPassType::Graphics,
            [position, normal, ambientOcclusion](FrameGraphBuilder& builder) {
                builder.readTexture(position, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                builder.readTexture(normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                builder.writeTexture(ambientOcclusion, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [this, commandBuffer, frameIndex](const FrameGraphContext&) {
                recordSsaoPass(commandBuffer, frameIndex);
            });

        frameGraph_.addPass(
            "Procedural sky", FrameGraphPassType::Graphics,
            [lighting](FrameGraphBuilder& builder) {
                builder.writeTexture(lighting, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [this, commandBuffer, frameIndex](const FrameGraphContext&) {
                recordSkyPass(commandBuffer, frameIndex);
            });

        frameGraph_.addPass(
            "Deferred lighting", FrameGraphPassType::Graphics,
            [position, normal, albedo, ambientOcclusion, shadows, lighting](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input : {position, normal, albedo, ambientOcclusion}) {
                    builder.readTexture(input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                }
                for (FrameGraphResourceHandle shadow : shadows) {
                    builder.readTexture(shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                        VK_IMAGE_ASPECT_DEPTH_BIT);
                }
                builder.writeTexture(lighting, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [this, commandBuffer, frameIndex](const FrameGraphContext&) {
                recordDeferredLightingPass(commandBuffer, frameIndex);
            });

        frameGraph_.addPass(
            "TAA resolve", FrameGraphPassType::Graphics,
            [lighting, motion, historyRead, taaResolved](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input : {lighting, motion, historyRead}) {
                    builder.readTexture(input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                }
                builder.writeTexture(taaResolved, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [this, commandBuffer, frameIndex](const FrameGraphContext&) {
                recordTaaPass(commandBuffer, frameIndex);
            });

        frameGraph_.addPass(
            "TAA history copy", FrameGraphPassType::Transfer,
            [taaResolved, historyWrite](FrameGraphBuilder& builder) {
                builder.readTexture(taaResolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT);
                builder.writeTexture(historyWrite, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_ACCESS_TRANSFER_WRITE_BIT);
            },
            [this, commandBuffer, frameIndex](const FrameGraphContext&) {
                recordHistoryCopy(commandBuffer, frameIndex);
            });

        frameGraph_.addPass(
            "TAA history ready", FrameGraphPassType::Transfer,
            [historyWrite](FrameGraphBuilder& builder) {
                builder.readTexture(historyWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            },
            nullptr);

        frameGraph_.addPass(
            "Tonemap", FrameGraphPassType::Graphics,
            [taaResolved, swap](FrameGraphBuilder& builder) {
                builder.readTexture(taaResolved, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                builder.writeTexture(swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [this, commandBuffer, frameIndex, imageIndex](const FrameGraphContext&) {
                recordTonemapPass(commandBuffer, frameIndex, imageIndex);
            });

        frameGraph_.addPass(
            "ImGui overlay", FrameGraphPassType::Graphics,
            [swap](FrameGraphBuilder& builder) {
                builder.writeTexture(swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [this, commandBuffer, imageIndex](const FrameGraphContext&) {
                imgui_.record(commandBuffer, context_.swapchainImageViews()[imageIndex], context_.swapchainExtent());
            });

        frameGraph_.addPass(
            "Present", FrameGraphPassType::Present,
            [swap](FrameGraphBuilder& builder) {
                builder.readTexture(swap, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
            },
            nullptr);

        FrameGraphContext graphContext;
        graphContext.device = context_.device();
        graphContext.commandBuffer = commandBuffer;
        graphContext.cmdBeginDebugUtilsLabel = context_.cmdBeginDebugUtilsLabel();
        graphContext.cmdEndDebugUtilsLabel = context_.cmdEndDebugUtilsLabel();
        graphContext.frameIndex = frameIndex;
        frameGraph_.execute(graphContext);
        checkVk(vkEndCommandBuffer(commandBuffer), "Failed to end LevelRenderer command buffer.");

        textures_.markHistoryValid(frameIndex);
        previousViewProjection_ = viewProjection;
        previousCameraPosition_ = camera.position();
        previousCameraForward_ = cameraForward;
        previousFieldOfView_ = camera.fieldOfViewDegrees();
        previousTaaEnabled_ = settings.enableTaa;
        hasPreviousCamera_ = true;
        ++frameNumber_;
    }

    void LevelRenderer::recordShadowPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                         std::uint32_t cascadeIndex, const glm::mat4& lightViewProjection) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = frame.shadowCascades[cascadeIndex].view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.extent = VkExtent2D{shadowMapResolution, shadowMapResolution};
        renderingInfo.layerCount = 1;
        renderingInfo.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        setViewportAndScissor(commandBuffer, VkExtent2D{shadowMapResolution, shadowMapResolution});
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordShadow(commandBuffer, frameIndex, cascadeIndex, lightViewProjection);
        }
        vkCmdEndRendering(commandBuffer);
    }

    void LevelRenderer::recordGBufferPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                          const glm::mat4& viewProjection, const glm::mat4& previousViewProjection) {
        const VkExtent2D extent = context_.swapchainExtent();
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        const std::array<VkClearValue, 4> colorClears = {
            VkClearValue{.color = {{0.0f, 0.0f, 0.0f, 0.0f}}},
            VkClearValue{.color = {{0.0f, 0.0f, 1.0f, 1.0f}}},
            VkClearValue{.color = {{0.0f, 0.0f, 0.0f, 1.0f}}},
            VkClearValue{.color = {{0.0f, 0.0f, 0.0f, 0.0f}}},
        };
        std::array<VkRenderingAttachmentInfo, 4> colorAttachments{};
        const std::array<const VulkanImage*, 4> images = {&frame.position, &frame.normalRoughness, &frame.albedo,
                                                          &frame.motion};
        for (std::uint32_t index = 0; index < images.size(); ++index) {
            colorAttachments[index].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachments[index].imageView = images[index]->view;
            colorAttachments[index].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachments[index].clearValue = colorClears[index];
        }
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = frame.depth.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.extent = extent;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<std::uint32_t>(colorAttachments.size());
        renderingInfo.pColorAttachments = colorAttachments.data();
        renderingInfo.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        setViewportAndScissor(commandBuffer, extent);
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordGBuffer(commandBuffer, frameIndex, viewProjection, previousViewProjection);
        }
        vkCmdEndRendering(commandBuffer);
    }

    void LevelRenderer::recordSsaoPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        recordFullscreen(commandBuffer, frame.ambientOcclusion.view, context_.swapchainExtent(), pipelines_.ssao(),
                         textures_.descriptorSet(frameIndex), VK_ATTACHMENT_LOAD_OP_CLEAR,
                         VkClearColorValue{{1.0f, 1.0f, 1.0f, 1.0f}});
    }

    void LevelRenderer::recordSkyPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        recordFullscreen(commandBuffer, frame.lighting.view, context_.swapchainExtent(), pipelines_.sky(),
                         textures_.descriptorSet(frameIndex), VK_ATTACHMENT_LOAD_OP_CLEAR,
                         VkClearColorValue{{0.035f, 0.04f, 0.05f, 1.0f}});
    }

    void LevelRenderer::recordDeferredLightingPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        recordFullscreen(commandBuffer, frame.lighting.view, context_.swapchainExtent(), pipelines_.deferredLighting(),
                         textures_.descriptorSet(frameIndex), VK_ATTACHMENT_LOAD_OP_LOAD);
    }

    void LevelRenderer::recordTaaPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        recordFullscreen(commandBuffer, frame.taaResolved.view, context_.swapchainExtent(), pipelines_.taa(),
                         textures_.descriptorSet(frameIndex), VK_ATTACHMENT_LOAD_OP_CLEAR);
    }

    void LevelRenderer::recordHistoryCopy(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) {
        const VkExtent2D extent = context_.swapchainExtent();
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        VkImageCopy copy{};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent = VkExtent3D{extent.width, extent.height, 1};
        vkCmdCopyImage(commandBuffer, frame.taaResolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       frame.history.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    void LevelRenderer::recordTonemapPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                          std::uint32_t imageIndex) {
        recordFullscreen(commandBuffer, context_.swapchainImageViews()[imageIndex], context_.swapchainExtent(),
                         pipelines_.tonemap(), textures_.descriptorSet(frameIndex), VK_ATTACHMENT_LOAD_OP_CLEAR,
                         VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
    }

} // namespace lumin::render

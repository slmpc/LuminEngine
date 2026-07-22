#include "lumin/render/LevelRenderer.hpp"

#include "lumin/platform/Window.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace lumin::render {
    namespace {

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

        FrameGraphTextureDesc textureDesc(const VulkanImage& image, VkExtent2D extent, VkImageUsageFlags usage) {
            FrameGraphTextureDesc desc;
            desc.width = extent.width;
            desc.height = extent.height;
            desc.format = image.format;
            desc.usage = usage;
            desc.image = image.image;
            desc.aspectMask = image.aspectMask;
            desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            desc.finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            return desc;
        }

    }

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

        const std::optional<VulkanFrame> frame = context_.beginFrame();
        if (!frame.has_value()) {
            refreshSwapchainResources();
            return;
        }

        imgui_.beginFrame();
        imgui_.drawLevelPanel(camera, settings, modelCount(), mdiDrawCount());
        recordCommandBuffer(frame->commandBuffer, frame->frameIndex, frame->imageIndex, camera);
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
        pipelines_.createPostprocess(textures_.descriptorSetLayout(), context_.swapchainFormat());
        const std::array<VkFormat, 3> colorFormats = {textures_.positionFormat(), textures_.normalFormat(),
                                                       textures_.albedoFormat()};
        modelRenderer_ = std::make_unique<ModelRenderer>(context_, level_, shaderDirectory_, colorFormats,
                                                         textures_.depthFormat(), TextureManager::maxFramesInFlight);
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
                                             std::uint32_t imageIndex, const scene::Camera& camera) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin LevelRenderer command buffer.");

        const VkExtent2D extent = context_.swapchainExtent();
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        frameGraph_.reset();
        const auto position = frameGraph_.importTexture(
            "gbuffer.position", textureDesc(frame.position, extent,
                                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto normal = frameGraph_.importTexture(
            "gbuffer.normal", textureDesc(frame.normalRoughness, extent,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto albedo = frameGraph_.importTexture(
            "gbuffer.albedo", textureDesc(frame.albedo, extent,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        const auto depth = frameGraph_.importTexture(
            "gbuffer.depth", textureDesc(frame.depth, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));

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

        frameGraph_.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [position, normal, albedo, depth](FrameGraphBuilder& builder) {
                builder.writeTexture(position, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                builder.writeTexture(normal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                builder.writeTexture(albedo, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                builder.writeTexture(depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            },
            [this, commandBuffer, frameIndex, &camera](const FrameGraphContext&) {
                recordGBufferPass(commandBuffer, frameIndex, camera);
            });

        frameGraph_.addPass(
            "Postprocess", FrameGraphPassType::Graphics,
            [position, normal, albedo, swap](FrameGraphBuilder& builder) {
                builder.readTexture(position, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
                builder.readTexture(normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
                builder.readTexture(albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
                builder.writeTexture(swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            },
            [this, commandBuffer, frameIndex, imageIndex](const FrameGraphContext&) {
                recordPostprocessPass(commandBuffer, frameIndex, imageIndex);
            });

        frameGraph_.addPass(
            "ImGui overlay", FrameGraphPassType::Graphics,
            [swap](FrameGraphBuilder& builder) {
                builder.writeTexture(swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            },
            [this, commandBuffer, imageIndex](const FrameGraphContext&) {
                imgui_.record(commandBuffer, context_.swapchainImageViews()[imageIndex], context_.swapchainExtent());
            });

        frameGraph_.addPass(
            "Present", FrameGraphPassType::Present,
            [swap](FrameGraphBuilder& builder) {
                builder.readTexture(swap, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
            },
            nullptr);

        FrameGraphContext graphContext;
        graphContext.device = context_.device();
        graphContext.commandBuffer = commandBuffer;
        graphContext.frameIndex = frameIndex;
        frameGraph_.execute(graphContext);
        checkVk(vkEndCommandBuffer(commandBuffer), "Failed to end LevelRenderer command buffer.");
    }

    void LevelRenderer::recordGBufferPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                          const scene::Camera& camera) {
        const VkExtent2D extent = context_.swapchainExtent();
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        const std::array<VkClearValue, 3> colorClears = {
            VkClearValue{.color = {{0.0f, 0.0f, 0.0f, 0.0f}}},
            VkClearValue{.color = {{0.0f, 0.0f, 1.0f, 1.0f}}},
            VkClearValue{.color = {{0.0f, 0.0f, 0.0f, 1.0f}}},
        };
        std::array<VkRenderingAttachmentInfo, 3> colorAttachments{};
        const VulkanImage* images[] = {&frame.position, &frame.normalRoughness, &frame.albedo};
        for (std::uint32_t index = 0; index < 3; ++index) {
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

        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(std::max(1U, extent.height));
        modelRenderer_->record(commandBuffer, frameIndex, camera, aspect);
        vkCmdEndRendering(commandBuffer);
    }

    void LevelRenderer::recordPostprocessPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                               std::uint32_t imageIndex) {
        const VkExtent2D extent = context_.swapchainExtent();
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = context_.swapchainImageViews()[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{0.035f, 0.04f, 0.05f, 1.0f}};
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.extent = extent;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        const GraphicsPipeline& pipeline = pipelines_.postprocess();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
        const VkDescriptorSet descriptorSet = textures_.descriptorSet(frameIndex);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
                                &descriptorSet, 0, nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRendering(commandBuffer);
    }

}

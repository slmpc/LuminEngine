#include "lumin/render/ObjRenderer.hpp"

#include "lumin/platform/Window.hpp"
#include "lumin/render/VulkanContext.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

#include <GLFW/glfw3.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>

namespace lumin::render {
    namespace {

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

        std::array<VkVertexInputAttributeDescription, 3> vertexAttributeDescriptions() {
            return {
                VkVertexInputAttributeDescription{
                    .location = 0,
                    .binding = 0,
                    .format = VK_FORMAT_R32G32B32_SFLOAT,
                    .offset = offsetof(assets::Vertex, position),
                },
                VkVertexInputAttributeDescription{
                    .location = 1,
                    .binding = 0,
                    .format = VK_FORMAT_R32G32B32_SFLOAT,
                    .offset = offsetof(assets::Vertex, normal),
                },
                VkVertexInputAttributeDescription{
                    .location = 2,
                    .binding = 0,
                    .format = VK_FORMAT_R32G32_SFLOAT,
                    .offset = offsetof(assets::Vertex, texCoord),
                },
            };
        }

    } // namespace

    ObjRenderer::ObjRenderer(platform::Window& window, VulkanContext& context, const assets::Mesh& mesh,
                             std::filesystem::path shaderDirectory)
        : window_(window), context_(context), resources_(context),
          shaders_(context.device(), std::move(shaderDirectory)), pipelineFactory_(context.device()), mesh_(mesh) {
        if (mesh_.empty()) {
            throw std::runtime_error("ObjRenderer requires a non-empty mesh.");
        }

        glm::vec3 minBounds{std::numeric_limits<float>::max()};
        glm::vec3 maxBounds{std::numeric_limits<float>::lowest()};
        for (const assets::Vertex& vertex : mesh_.vertices) {
            minBounds = glm::min(minBounds, vertex.position);
            maxBounds = glm::max(maxBounds, vertex.position);
        }

        meshCenter_ = (minBounds + maxBounds) * 0.5f;
        float radius = 0.0f;
        for (const assets::Vertex& vertex : mesh_.vertices) {
            radius = std::max(radius, glm::length(vertex.position - meshCenter_));
        }
        meshScale_ = radius > 0.0001f ? 1.35f / radius : 1.0f;

        createDescriptorSetLayout();
        createMeshBuffers();
        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
        createSwapchainResources();
        createCommandBuffers();
        createSyncObjects();
        initImGui();
    }

    ObjRenderer::~ObjRenderer() {
        waitIdle();
        shutdownImGui();
        cleanupSwapchainResources();

        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_.device(), descriptorPool_, nullptr);
        }

        if (descriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context_.device(), descriptorSetLayout_, nullptr);
        }

        for (VulkanBuffer& buffer : uniformBuffers_) {
            resources_.destroyBuffer(buffer);
        }

        resources_.destroyBuffer(indexBuffer_);
        resources_.destroyBuffer(vertexBuffer_);

        for (std::uint32_t i = 0; i < maxFramesInFlight; ++i) {
            if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(context_.device(), renderFinishedSemaphores_[i], nullptr);
            }

            if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(context_.device(), imageAvailableSemaphores_[i], nullptr);
            }

            if (inFlightFences_[i] != VK_NULL_HANDLE) {
                vkDestroyFence(context_.device(), inFlightFences_[i], nullptr);
            }
        }
    }

    void ObjRenderer::drawFrame(RenderSettings& settings) {
        vkWaitForFences(context_.device(), 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

        std::uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(context_.device(), swapchain_, UINT64_MAX,
                                                imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("Failed to acquire swapchain image.");
        }

        vkResetFences(context_.device(), 1, &inFlightFences_[currentFrame_]);
        vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);

        updateUniformBuffer(currentFrame_, settings);

        imgui_.newFrame();
        drawSettingsUi(settings);

        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex);

        const VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
        const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        const VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        checkVk(vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, inFlightFences_[currentFrame_]),
                "Failed to submit draw command buffer.");

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || window_.framebufferResized()) {
            window_.resetFramebufferResized();
            recreateSwapchain();
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to present swapchain image.");
        }

        currentFrame_ = (currentFrame_ + 1) % maxFramesInFlight;
    }

    void ObjRenderer::waitIdle() const {
        if (context_.device() != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(context_.device());
        }
    }

    void ObjRenderer::createSwapchainResources() {
        createSwapchain();
        createImageViews();
        createDepthResources();
        createGraphicsPipeline();
    }

    void ObjRenderer::cleanupSwapchainResources() {
        resources_.destroyImage(depthImage_);

        pipelineFactory_.destroy(graphicsPipeline_);

        for (VkImageView imageView : swapchainImageViews_) {
            vkDestroyImageView(context_.device(), imageView, nullptr);
        }
        swapchainImageViews_.clear();
        swapchainImages_.clear();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(context_.device(), swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    void ObjRenderer::recreateSwapchain() {
        waitIdle();
        shutdownImGui();
        cleanupSwapchainResources();
        createSwapchainResources();
        initImGui();
    }

    void ObjRenderer::createSwapchain() {
        const SwapchainSupport support = querySwapchainSupport();
        const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
        const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
        const VkExtent2D extent = chooseExtent(support.capabilities);

        minImageCount_ = std::max(2U, support.capabilities.minImageCount);
        std::uint32_t imageCount = minImageCount_ + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = context_.surface();
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        const std::uint32_t queueFamilyIndices[] = {
            context_.graphicsQueueFamily(),
            context_.presentQueueFamily(),
        };

        if (context_.graphicsQueueFamily() != context_.presentQueueFamily()) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        checkVk(vkCreateSwapchainKHR(context_.device(), &createInfo, nullptr, &swapchain_),
                "Failed to create swapchain.");

        vkGetSwapchainImagesKHR(context_.device(), swapchain_, &imageCount, nullptr);
        swapchainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(context_.device(), swapchain_, &imageCount, swapchainImages_.data());

        swapchainImageFormat_ = surfaceFormat.format;
        swapchainExtent_ = extent;
    }

    void ObjRenderer::createImageViews() {
        swapchainImageViews_.resize(swapchainImages_.size());

        for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
            swapchainImageViews_[i] =
                resources_.createImageView(swapchainImages_[i], swapchainImageFormat_, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    void ObjRenderer::createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding uniformBinding{};
        uniformBinding.binding = 0;
        uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniformBinding.descriptorCount = 1;
        uniformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        createInfo.bindingCount = 1;
        createInfo.pBindings = &uniformBinding;

        checkVk(vkCreateDescriptorSetLayout(context_.device(), &createInfo, nullptr, &descriptorSetLayout_),
                "Failed to create descriptor set layout.");
    }

    void ObjRenderer::createGraphicsPipeline() {
        const VkShaderModule vertexShader = shaders_.loadModule("blinn_phong.vert.spv");
        const VkShaderModule fragmentShader = shaders_.loadModule("blinn_phong.frag.spv");

        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(assets::Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        const auto attributes = vertexAttributeDescriptions();

        const std::array<VkVertexInputBindingDescription, 1> bindings = {
            bindingDescription,
        };

        GraphicsPipelineDesc desc;
        desc.vertexShader = vertexShader;
        desc.fragmentShader = fragmentShader;
        desc.descriptorSetLayout = descriptorSetLayout_;
        desc.colorFormat = swapchainImageFormat_;
        desc.depthFormat = depthImage_.format;
        desc.vertexBindings = bindings;
        desc.vertexAttributes = attributes;
        graphicsPipeline_ = pipelineFactory_.createGraphicsPipeline(desc);

        vkDestroyShaderModule(context_.device(), fragmentShader, nullptr);
        vkDestroyShaderModule(context_.device(), vertexShader, nullptr);
    }

    void ObjRenderer::createDepthResources() {
        depthImage_ = resources_.createImage(swapchainExtent_.width, swapchainExtent_.height, findDepthFormat(),
                                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void ObjRenderer::createCommandBuffers() {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = context_.commandPool();
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());

        checkVk(vkAllocateCommandBuffers(context_.device(), &allocateInfo, commandBuffers_.data()),
                "Failed to allocate command buffers.");
    }

    void ObjRenderer::createSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (std::uint32_t i = 0; i < maxFramesInFlight; ++i) {
            checkVk(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]),
                    "Failed to create image available semaphore.");
            checkVk(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]),
                    "Failed to create render finished semaphore.");
            checkVk(vkCreateFence(context_.device(), &fenceInfo, nullptr, &inFlightFences_[i]),
                    "Failed to create in-flight fence.");
        }
    }

    void ObjRenderer::createMeshBuffers() {
        const VkDeviceSize vertexSize = sizeof(assets::Vertex) * mesh_.vertices.size();
        vertexBuffer_ =
            resources_.createBuffer(vertexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        resources_.writeBuffer(vertexBuffer_, mesh_.vertices.data(), vertexSize);

        const VkDeviceSize indexSize = sizeof(std::uint32_t) * mesh_.indices.size();
        indexBuffer_ =
            resources_.createBuffer(indexSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        resources_.writeBuffer(indexBuffer_, mesh_.indices.data(), indexSize);
    }

    void ObjRenderer::createUniformBuffers() {
        for (VulkanBuffer& buffer : uniformBuffers_) {
            buffer =
                resources_.createBuffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
    }

    void ObjRenderer::createDescriptorPool() {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = maxFramesInFlight;

        VkDescriptorPoolCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        createInfo.poolSizeCount = 1;
        createInfo.pPoolSizes = &poolSize;
        createInfo.maxSets = maxFramesInFlight;

        checkVk(vkCreateDescriptorPool(context_.device(), &createInfo, nullptr, &descriptorPool_),
                "Failed to create descriptor pool.");
    }

    void ObjRenderer::createDescriptorSets() {
        std::array<VkDescriptorSetLayout, maxFramesInFlight> layouts{};
        layouts.fill(descriptorSetLayout_);

        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = descriptorPool_;
        allocateInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocateInfo.pSetLayouts = layouts.data();

        checkVk(vkAllocateDescriptorSets(context_.device(), &allocateInfo, descriptorSets_.data()),
                "Failed to allocate descriptor sets.");

        for (std::uint32_t i = 0; i < maxFramesInFlight; ++i) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers_[i].buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(FrameUniforms);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSets_[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
        }
    }

    void ObjRenderer::initImGui() {
        ImGuiLayerConfig config;
        config.apiVersion = context_.apiVersion();
        config.instance = context_.instance();
        config.physicalDevice = context_.physicalDevice();
        config.device = context_.device();
        config.queueFamily = context_.graphicsQueueFamily();
        config.queue = context_.graphicsQueue();
        config.minImageCount = minImageCount_;
        config.imageCount = static_cast<std::uint32_t>(swapchainImages_.size());
        config.colorFormat = swapchainImageFormat_;
        config.depthFormat = depthImage_.format;
        config.enableKeyboard = true;
        config.enableDocking = false;
        imgui_.initialize(window_, config);
    }

    void ObjRenderer::shutdownImGui() {
        imgui_.shutdown();
    }

    void ObjRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin recording command buffer.");

        frameGraph_.reset();

        FrameGraphTextureDesc backBufferDesc;
        backBufferDesc.width = swapchainExtent_.width;
        backBufferDesc.height = swapchainExtent_.height;
        backBufferDesc.format = swapchainImageFormat_;
        backBufferDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        backBufferDesc.image = swapchainImages_[imageIndex];
        backBufferDesc.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        backBufferDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        backBufferDesc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        FrameGraphTextureDesc depthDesc;
        depthDesc.width = swapchainExtent_.width;
        depthDesc.height = swapchainExtent_.height;
        depthDesc.format = depthImage_.format;
        depthDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depthDesc.image = depthImage_.image;
        depthDesc.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        const FrameGraphResourceHandle backBuffer = frameGraph_.importTexture("swapchain.backbuffer", backBufferDesc);
        const FrameGraphResourceHandle depth = frameGraph_.importTexture("scene.depth", depthDesc);

        frameGraph_.addPass(
            "OBJ + ImGui dynamic rendering", FrameGraphPassType::Graphics,
            [backBuffer, depth](FrameGraphBuilder& builder) {
                builder.writeTexture(backBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                builder.writeTexture(depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            },
            [this, commandBuffer, imageIndex](const FrameGraphContext&) {
                recordScenePass(commandBuffer, imageIndex);
            });

        frameGraph_.addPass(
            "Present", FrameGraphPassType::Present,
            [backBuffer](FrameGraphBuilder& builder) {
                builder.readTexture(backBuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    0, VK_IMAGE_ASPECT_COLOR_BIT);
            },
            nullptr);

        FrameGraphContext graphContext;
        graphContext.device = context_.device();
        graphContext.commandBuffer = commandBuffer;
        graphContext.frameIndex = currentFrame_;
        frameGraph_.execute(graphContext);

        checkVk(vkEndCommandBuffer(commandBuffer), "Failed to record command buffer.");
    }

    void ObjRenderer::recordScenePass(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) {
        VkClearValue colorClear{};
        colorClear.color = {{0.035f, 0.04f, 0.05f, 1.0f}};

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchainImageViews_[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = colorClear;

        VkClearValue depthClear{};
        depthClear.depthStencil = {1.0f, 0};

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depthImage_.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue = depthClear;

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.offset = {0, 0};
        renderingInfo.renderArea.extent = swapchainExtent_;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(commandBuffer, &renderingInfo);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapchainExtent_.width);
        viewport.height = static_cast<float>(swapchainExtent_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapchainExtent_;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_.pipeline);

        const VkBuffer vertexBuffers[] = {vertexBuffer_.buffer};
        const VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_.layout, 0, 1,
                                &descriptorSets_[currentFrame_], 0, nullptr);
        vkCmdDrawIndexed(commandBuffer, static_cast<std::uint32_t>(mesh_.indices.size()), 1, 0, 0, 0);

        imgui_.render(commandBuffer);

        vkCmdEndRendering(commandBuffer);
    }

    void ObjRenderer::updateUniformBuffer(std::uint32_t frameIndex, const RenderSettings& settings) {
        const float aspect =
            static_cast<float>(swapchainExtent_.width) / static_cast<float>(std::max(1U, swapchainExtent_.height));

        FrameUniforms uniforms;
        uniforms.model =
            glm::scale(glm::mat4{1.0f}, glm::vec3{meshScale_}) * glm::translate(glm::mat4{1.0f}, -meshCenter_);
        uniforms.view = glm::lookAt(settings.cameraPosition, glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
        uniforms.projection = glm::perspective(glm::radians(60.0f), aspect, 0.05f, 100.0f);
        uniforms.projection[1][1] *= -1.0f;
        uniforms.cameraPosition = glm::vec4{settings.cameraPosition, 1.0f};
        uniforms.lightPosition = glm::vec4{settings.lightPosition, 1.0f};
        uniforms.lightColor = glm::vec4{settings.lightColor, 1.0f};
        uniforms.materialColor = glm::vec4{settings.materialColor, 1.0f};
        uniforms.materialParams = glm::vec4{settings.ambientStrength, settings.specularStrength, settings.shininess,
                                            settings.smoothShading ? 1.0f : 0.0f};

        resources_.writeBuffer(uniformBuffers_[frameIndex], &uniforms, sizeof(uniforms));
    }

    void ObjRenderer::drawSettingsUi(RenderSettings& settings) {
        ImGui::Begin("Lumin Renderer");
        ImGui::Text("Mesh: %s", mesh_.name.c_str());
        ImGui::Text("Vertices: %zu", mesh_.vertices.size());
        ImGui::Text("Triangles: %zu", mesh_.indices.size() / 3);
        ImGui::Separator();
        ImGui::SliderFloat3("Camera", &settings.cameraPosition.x, -8.0f, 8.0f);
        ImGui::SliderFloat3("Light", &settings.lightPosition.x, -8.0f, 8.0f);
        ImGui::ColorEdit3("Light Color", &settings.lightColor.x);
        ImGui::ColorEdit3("Material", &settings.materialColor.x);
        ImGui::SliderFloat("Ambient", &settings.ambientStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Specular", &settings.specularStrength, 0.0f, 2.0f);
        ImGui::SliderFloat("Shininess", &settings.shininess, 1.0f, 256.0f, "%.0f");
        ImGui::Checkbox("Smooth Shading", &settings.smoothShading);
        ImGui::Checkbox("ImGui Demo", &settings.showDemoWindow);
        ImGui::End();

        if (settings.showDemoWindow) {
            ImGui::ShowDemoWindow(&settings.showDemoWindow);
        }
    }

    ObjRenderer::SwapchainSupport ObjRenderer::querySwapchainSupport() const {
        SwapchainSupport support;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice(), context_.surface(), &support.capabilities);

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), context_.surface(), &formatCount, nullptr);
        support.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), context_.surface(), &formatCount,
                                             support.formats.data());

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice(), context_.surface(), &presentModeCount,
                                                  nullptr);
        support.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice(), context_.surface(), &presentModeCount,
                                                  support.presentModes.data());

        return support;
    }

    VkSurfaceFormatKHR ObjRenderer::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
        for (const VkSurfaceFormatKHR& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }

        if (formats.empty()) {
            throw std::runtime_error("Swapchain surface does not expose any formats.");
        }

        return formats.front();
    }

    VkPresentModeKHR ObjRenderer::choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const {
        for (const VkPresentModeKHR presentMode : presentModes) {
            if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return presentMode;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D ObjRenderer::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        VkExtent2D extent = window_.framebufferExtent();
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height =
            std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return extent;
    }

    VkFormat ObjRenderer::findDepthFormat() const {
        return resources_.findDepthFormat();
    }

} // namespace lumin::render

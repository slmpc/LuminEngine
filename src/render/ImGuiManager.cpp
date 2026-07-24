#include "lumin/render/ImGuiManager.hpp"

#include "lumin/platform/Window.hpp"
#include "lumin/render/VulkanContext.hpp"

#include <imgui.h>

namespace lumin::render {

    ImGuiManager::~ImGuiManager() {
        shutdown();
    }

    void ImGuiManager::initialize(platform::Window& window, VulkanContext& context) {
        shutdown();
        ImGuiLayerConfig config;
        config.apiVersion = context.apiVersion();
        config.instance = context.instance();
        config.physicalDevice = context.physicalDevice();
        config.device = context.device();
        config.queueFamily = context.graphicsQueueFamily();
        config.queue = context.graphicsQueue();
        config.minImageCount = context.swapchainMinImageCount();
        config.imageCount = context.swapchainImageCount();
        config.colorFormat = context.swapchainFormat();
        config.depthFormat = VK_FORMAT_UNDEFINED;
        config.enableKeyboard = true;
        config.enableDocking = true;
        layer_.initialize(window, config);
    }

    void ImGuiManager::shutdown() {
        cancelFrame();
        layer_.shutdown();
    }

    void ImGuiManager::beginFrame(ImGuiContent* content) {
        if (!layer_.initialized() || framePrepared_) {
            return;
        }
        layer_.newFrame();
        framePrepared_ = true;
        try {
            if (content != nullptr) {
                content->draw();
            }
        } catch (...) {
            cancelFrame();
            throw;
        }
    }

    void ImGuiManager::cancelFrame() noexcept {
        if (!framePrepared_) {
            return;
        }
        ImGui::EndFrame();
        framePrepared_ = false;
    }

    void ImGuiManager::record(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D extent) {
        if (!layer_.initialized() || !framePrepared_) {
            return;
        }
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = targetView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.extent = extent;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        layer_.render(commandBuffer);
        framePrepared_ = false;
        vkCmdEndRendering(commandBuffer);
    }

    bool ImGuiManager::framePrepared() const noexcept {
        return framePrepared_;
    }

    ImGuiCaptureState ImGuiManager::captureState() const noexcept {
        return layer_.captureState();
    }

} // namespace lumin::render

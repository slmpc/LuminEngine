#include "lumin/render/ImGuiManager.hpp"

#include "lumin/platform/Window.hpp"
#include "lumin/render/VulkanContext.hpp"

namespace lumin::render {

    ImGuiManager::~ImGuiManager() {
        shutdown();
    }

    void ImGuiManager::initialize(platform::Window& window, VulkanContext& context) {
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
        layer_.shutdown();
    }

    void ImGuiManager::beginFrame(ImGuiContent* content) {
        if (!layer_.initialized()) {
            return;
        }
        layer_.newFrame();
        if (content != nullptr) {
            content->draw();
        }
    }

    void ImGuiManager::record(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D extent) {
        if (!layer_.initialized()) {
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
        vkCmdEndRendering(commandBuffer);
    }

    ImGuiCaptureState ImGuiManager::captureState() const noexcept {
        return layer_.captureState();
    }

} // namespace lumin::render

#include "lumin/render/ImGuiManager.hpp"

#include "lumin/platform/Window.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/scene/Camera.hpp"

#include <imgui.h>

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
        layer_.initialize(window, config);
    }

    void ImGuiManager::shutdown() {
        layer_.shutdown();
    }

    void ImGuiManager::beginFrame() {
        layer_.newFrame();
    }

    void ImGuiManager::drawLevelPanel(scene::Camera& camera, RenderSettings& settings, std::uint32_t modelCount,
                                      std::uint32_t mdiDrawCount) {
        settings.cameraPosition = camera.position();
        ImGui::SetNextWindowSize(ImVec2{360.0f, 320.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        ImGui::Begin("Lumin Level Renderer");
        ImGui::Text("Models: %u", modelCount);
        ImGui::Text("MDI draws: %u", mdiDrawCount);
        ImGui::Text("G-buffer: Position + Normal + Albedo + Motion");
        ImGui::Text("Lighting: 4-cascade CSM + SSAO + Sky");
        ImGui::Text("Postprocess: TAA + Tonemap");
        ImGui::Text("Camera: %.2f, %.2f, %.2f", camera.position().x, camera.position().y, camera.position().z);
        float moveSpeed = camera.moveSpeed();
        if (ImGui::SliderFloat("Camera speed", &moveSpeed, 0.1f, 20.0f)) {
            camera.setMoveSpeed(moveSpeed);
        }
        ImGui::SliderFloat3("Camera position", &settings.cameraPosition.x, -20.0f, 20.0f);
        camera.setPosition(settings.cameraPosition);
        ImGui::Separator();
        ImGui::Checkbox("Cascaded shadows", &settings.enableShadows);
        ImGui::Checkbox("Ambient occlusion", &settings.enableSsao);
        ImGui::Checkbox("Temporal anti-aliasing", &settings.enableTaa);
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.1f, 4.0f);
        ImGui::SliderFloat3("Sun direction", &settings.sunDirection.x, -1.0f, 1.0f);
        ImGui::End();
    }

    void ImGuiManager::record(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D extent) {
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

} // namespace lumin::render

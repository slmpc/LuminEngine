#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace lumin::platform {
    class Window;
}

namespace lumin::render {

    struct ImGuiLayerConfig {
        std::uint32_t apiVersion = VK_API_VERSION_1_3;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        std::uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;
        std::uint32_t minImageCount = 2;
        std::uint32_t imageCount = 2;
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        std::uint32_t samplerDescriptorCount = 8;
        std::uint32_t sampledImageDescriptorCount = 32;
        std::uint32_t maxDescriptorSets = 40;
        bool enableKeyboard = true;
        bool enableGamepad = false;
        bool enableDocking = false;
        float globalScale = 1.0f;
    };

    class ImGuiLayer {
    public:
        ImGuiLayer() = default;
        ~ImGuiLayer();

        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;

        void initialize(platform::Window& window, const ImGuiLayerConfig& config);
        void shutdown();
        void newFrame();
        void render(VkCommandBuffer commandBuffer);

        [[nodiscard]] bool initialized() const noexcept;

    private:
        platform::Window* window_ = nullptr;
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
        bool initialized_ = false;
    };

} // namespace lumin::render

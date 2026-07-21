#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace lumin::platform {
    class Window;
}

namespace lumin::render {

    struct VulkanContextDesc {
        std::string applicationName = "Lumin Engine";
        bool enableValidation = true;
    };

    class VulkanContext {
    public:
        VulkanContext(platform::Window& window, const VulkanContextDesc& desc);
        ~VulkanContext();

        VulkanContext(const VulkanContext&) = delete;
        VulkanContext& operator=(const VulkanContext&) = delete;

        [[nodiscard]] VkInstance instance() const noexcept;
        [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] VkSurfaceKHR surface() const noexcept;
        [[nodiscard]] VkQueue graphicsQueue() const noexcept;
        [[nodiscard]] VkQueue presentQueue() const noexcept;
        [[nodiscard]] VkCommandPool commandPool() const noexcept;
        [[nodiscard]] std::uint32_t graphicsQueueFamily() const noexcept;
        [[nodiscard]] std::uint32_t presentQueueFamily() const noexcept;
        [[nodiscard]] std::uint32_t apiVersion() const noexcept;

    private:
        struct QueueFamilyIndices {
            std::optional<std::uint32_t> graphics;
            std::optional<std::uint32_t> present;

            [[nodiscard]] bool complete() const noexcept;
        };

        void createInstance();
        void createDebugMessenger();
        void createSurface();
        void pickPhysicalDevice();
        void createDevice();
        void createCommandPool();

        [[nodiscard]] bool validationLayersAvailable() const;
        [[nodiscard]] std::vector<const char*> requiredExtensions() const;
        [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
        [[nodiscard]] bool deviceExtensionsAvailable(VkPhysicalDevice device) const;
        [[nodiscard]] bool isDeviceSuitable(VkPhysicalDevice device) const;

        platform::Window& window_;
        VulkanContextDesc desc_;
        bool validationEnabled_ = false;
        std::uint32_t apiVersion_ = VK_API_VERSION_1_0;

        VkInstance instance_ = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphicsQueue_ = VK_NULL_HANDLE;
        VkQueue presentQueue_ = VK_NULL_HANDLE;
        VkCommandPool commandPool_ = VK_NULL_HANDLE;
        QueueFamilyIndices queueFamilies_;
    };

} // namespace lumin::render

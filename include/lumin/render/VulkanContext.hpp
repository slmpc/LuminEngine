#pragma once

#include <array>
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

    struct VulkanFrame {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        std::uint32_t frameIndex = 0;
        std::uint32_t imageIndex = 0;
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
        [[nodiscard]] VkSwapchainKHR swapchain() const noexcept;
        [[nodiscard]] VkFormat swapchainFormat() const noexcept;
        [[nodiscard]] VkExtent2D swapchainExtent() const noexcept;
        [[nodiscard]] std::uint32_t swapchainMinImageCount() const noexcept;
        [[nodiscard]] std::uint32_t swapchainImageCount() const noexcept;
        [[nodiscard]] const std::vector<VkImage>& swapchainImages() const noexcept;
        [[nodiscard]] const std::vector<VkImageView>& swapchainImageViews() const noexcept;
        [[nodiscard]] std::uint64_t swapchainGeneration() const noexcept;

        // 帧同步、交换链获取和提交统一由 Context 管理，renderer 只记录命令。
        [[nodiscard]] std::optional<VulkanFrame> beginFrame();
        [[nodiscard]] bool submitFrame(const VulkanFrame& frame);
        void waitIdle() const;

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
        void createSwapchainResources();
        void cleanupSwapchainResources();
        void recreateSwapchain();
        void createSwapchain();
        void createImageViews();
        void createCommandBuffers();
        void createSyncObjects();
        void createRenderFinishedSemaphores();

        [[nodiscard]] bool validationLayersAvailable() const;
        [[nodiscard]] std::vector<const char*> requiredExtensions() const;
        [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
        [[nodiscard]] bool deviceExtensionsAvailable(VkPhysicalDevice device) const;
        [[nodiscard]] bool isDeviceSuitable(VkPhysicalDevice device) const;
        [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
        [[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
        [[nodiscard]] VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

        struct SwapchainSupport {
            VkSurfaceCapabilitiesKHR capabilities{};
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };
        [[nodiscard]] SwapchainSupport querySwapchainSupport() const;

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

        static constexpr std::uint32_t maxFramesInFlight = 2;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkFormat swapchainImageFormat_ = VK_FORMAT_UNDEFINED;
        VkExtent2D swapchainExtent_{};
        std::uint32_t minImageCount_ = 2;
        std::vector<VkImage> swapchainImages_;
        std::vector<VkImageView> swapchainImageViews_;
        std::array<VkCommandBuffer, maxFramesInFlight> commandBuffers_{};
        std::array<VkSemaphore, maxFramesInFlight> imageAvailableSemaphores_{};
        std::vector<VkSemaphore> renderFinishedSemaphores_;
        std::array<VkFence, maxFramesInFlight> inFlightFences_{};
        std::uint32_t currentFrame_ = 0;
        std::uint64_t swapchainGeneration_ = 0;
    };

} // namespace lumin::render

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.h>

namespace lumin::platform {
    class Window;
}

namespace lumin::render {

    struct ModelRendererCapabilities;

    struct VulkanContextDesc {
        std::string applicationName = "Lumin Engine";
        bool enableValidation = true;
    };

    struct VulkanFrame {
        nvrhi::CommandListHandle commandList;
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
        [[nodiscard]] std::uint32_t graphicsQueueFamily() const noexcept;
        [[nodiscard]] std::uint32_t presentQueueFamily() const noexcept;
        [[nodiscard]] std::uint32_t apiVersion() const noexcept;
        [[nodiscard]] VkSwapchainKHR swapchain() const noexcept;
        [[nodiscard]] VkFormat swapchainFormat() const noexcept;
        [[nodiscard]] VkExtent2D swapchainExtent() const noexcept;
        [[nodiscard]] std::uint32_t swapchainWidth() const noexcept;
        [[nodiscard]] std::uint32_t swapchainHeight() const noexcept;
        [[nodiscard]] nvrhi::Format swapchainRhiFormat() const noexcept;
        [[nodiscard]] bool swapchainIsSrgb() const noexcept;
        [[nodiscard]] std::uint32_t swapchainMinImageCount() const noexcept;
        [[nodiscard]] std::uint32_t swapchainImageCount() const noexcept;
        [[nodiscard]] const std::vector<VkImage>& swapchainImages() const noexcept;
        [[nodiscard]] const std::vector<VkImageView>& swapchainImageViews() const noexcept;
        [[nodiscard]] const nvrhi::vulkan::DeviceHandle& rhiDevice() const noexcept;
        [[nodiscard]] const std::vector<nvrhi::TextureHandle>& swapchainTextures() const noexcept;
        [[nodiscard]] nvrhi::ResourceStates swapchainTextureInitialState(std::uint32_t imageIndex) const;
        [[nodiscard]] ModelRendererCapabilities modelRendererCapabilities() const noexcept;
        [[nodiscard]] std::uint64_t swapchainGeneration() const noexcept;
        [[nodiscard]] PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginDebugUtilsLabel() const noexcept;
        [[nodiscard]] PFN_vkCmdEndDebugUtilsLabelEXT cmdEndDebugUtilsLabel() const noexcept;

        [[nodiscard]] static constexpr nvrhi::Format mapSwapchainFormat(VkFormat format) noexcept {
            switch (format) {
            case VK_FORMAT_B8G8R8A8_SRGB:
                return nvrhi::Format::SBGRA8_UNORM;
            case VK_FORMAT_B8G8R8A8_UNORM:
                return nvrhi::Format::BGRA8_UNORM;
            default:
                return nvrhi::Format::UNKNOWN;
            }
        }

        // 帧同步、交换链获取和提交统一由 Context 管理，renderer 只记录命令。
        [[nodiscard]] std::optional<VulkanFrame> beginFrame();
        [[nodiscard]] bool submitFrame(const VulkanFrame& frame);
        void cancelFrame(const VulkanFrame& frame);
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
        void createRhiDevice();
        void loadDebugUtilsFunctions();
        void createSwapchainResources();
        void cleanupSwapchainResources();
        void recreateSwapchain();
        void createSwapchain();
        void createImageViews();
        void createSwapchainTextures();
        void createFrameResources();
        void createRenderFinishedSemaphores();
        void destroy() noexcept;

        [[nodiscard]] bool validationLayersAvailable() const;
        [[nodiscard]] bool instanceExtensionAvailable(const char* extensionName) const;
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
        bool debugUtilsEnabled_ = false;
        std::uint32_t apiVersion_ = VK_API_VERSION_1_0;

        VkInstance instance_ = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
        PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginDebugUtilsLabel_ = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT cmdEndDebugUtilsLabel_ = nullptr;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphicsQueue_ = VK_NULL_HANDLE;
        VkQueue presentQueue_ = VK_NULL_HANDLE;
        nvrhi::vulkan::DeviceHandle rhiDevice_;
        QueueFamilyIndices queueFamilies_;

        static constexpr std::uint32_t maxFramesInFlight = 2;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkFormat swapchainImageFormat_ = VK_FORMAT_UNDEFINED;
        VkExtent2D swapchainExtent_{};
        std::uint32_t minImageCount_ = 2;
        std::vector<VkImage> swapchainImages_;
        std::vector<VkImageView> swapchainImageViews_;
        std::vector<nvrhi::TextureHandle> swapchainTextures_;
        std::vector<bool> swapchainTextureInitialized_;
        std::array<nvrhi::CommandListHandle, maxFramesInFlight> commandLists_{};
        std::array<nvrhi::EventQueryHandle, maxFramesInFlight> frameQueries_{};
        std::array<bool, maxFramesInFlight> frameQueryPending_{};
        std::array<VkSemaphore, maxFramesInFlight> imageAvailableSemaphores_{};
        std::vector<VkSemaphore> renderFinishedSemaphores_;
        std::uint32_t currentFrame_ = 0;
        std::uint64_t swapchainGeneration_ = 0;
        bool destroyed_ = false;
    };

} // namespace lumin::render

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.h>

#include "render/core/ModelRendererCapabilities.hpp"
#include "render/core/RenderFramePacket.hpp"
#include "render/platform/vulkan/VulkanRayTracingCapabilities.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::render {

    /** Vulkan 后端创建参数。 */
    struct VulkanContextDesc {
        /** 写入 Vulkan instance 的应用名称。 */
        std::string applicationName = "Lumin Engine";
        /** 请求启用可用的 Vulkan validation layer。 */
        bool enableValidation = true;

        /// 控制 Vulkan RT 运行阶段策略；`build` 字段由 CMake 生成配置覆盖，调用方不能修改构建能力。
        RayTracingPolicy rayTracing;
    };

    /** 一次可提交 Vulkan 帧的 NvRHI 命令列表和帧槽身份。 */
    struct VulkanFrame {
        /** 由当前 Context 创建且已经打开的命令列表。 */
        nvrhi::CommandListHandle commandList;
        /** 当前 in-flight frame slot。 */
        std::uint32_t frameIndex = 0;
        /** 本帧 acquire 到的交换链图像。 */
        std::uint32_t imageIndex = 0;
    };

    /**
     * @brief 主线程创建的 Vulkan instance 与 SDL window surface 所有权包。
     *
     * SDL 要求 surface bootstrap 与窗口事件生命周期保持在主线程。构造完成后必须立即移动给 `Renderer`；
     * 除析构外不公开原生句柄，避免主线程在移交后继续访问 Vulkan 对象。
     */
    class VulkanSurfaceBootstrap final {
    public:
        /**
         * @brief 为窗口创建 Vulkan instance 和 SDL surface。
         * @param window surface 所属窗口；只在构造期间访问，不会持久保存。
         * @param desc Vulkan instance 与设备策略描述。
         * @throws std::runtime_error Vulkan 1.3、instance 或 SDL surface 创建失败时抛出。
         * @thread_safety 必须在拥有 SDL window 的主线程调用。
         */
        VulkanSurfaceBootstrap(platform::Window& window, VulkanContextDesc desc);
        /** 释放尚未移交的 surface 和 instance。 */
        ~VulkanSurfaceBootstrap();

        VulkanSurfaceBootstrap(VulkanSurfaceBootstrap&&) noexcept;
        VulkanSurfaceBootstrap& operator=(VulkanSurfaceBootstrap&&) noexcept;
        VulkanSurfaceBootstrap(const VulkanSurfaceBootstrap&) = delete;
        VulkanSurfaceBootstrap& operator=(const VulkanSurfaceBootstrap&) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend class VulkanContext;
    };

    /**
     * @brief 渲染主线程独占的 Vulkan 1.3/NvRHI 设备、交换链与提交上下文。
     *
     * 构造后所有方法（只读 capability accessor 除外）均只能在构造线程调用。该类型是引擎唯一允许直接使用
     * 原生 Vulkan device、queue、swapchain 和同步对象的边界。
     */
    class VulkanContext {
    public:
        /**
         * @brief 接收当前线程的 bootstrap，并在渲染主线程创建设备、NvRHI 与交换链。
         * @param bootstrap 唯一的
         * instance/surface 所有权；构造开始后即被消费。
         * @throws std::exception 设备能力不足或任一后端资源创建失败时抛出。
         * @thread_safety 必须在拥有 SDL 窗口的渲染主线程构造和销毁。
         */
        explicit VulkanContext(VulkanSurfaceBootstrap bootstrap);
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
        /// 返回所选物理设备探测到的 RT 硬件能力；它不代表 RT 已被策略启用。
        [[nodiscard]] const VulkanRayTracingSupport& rayTracingSupport() const noexcept;
        /// 返回 RT 的最终策略决策；只有 `enabled()` 为真时才允许创建 RT 资源。
        [[nodiscard]] const RayTracingDecision& rayTracingDecision() const noexcept;
        [[nodiscard]] std::uint64_t swapchainGeneration() const noexcept;
        [[nodiscard]] PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginDebugUtilsLabel() const noexcept;
        [[nodiscard]] PFN_vkCmdEndDebugUtilsLabelEXT cmdEndDebugUtilsLabel() const noexcept;

        /**
         * @brief 更新交换链下一次 acquire/recreate 使用的窗口值状态。
         * @param state
         * 主线程随
         * packet 深拷贝的状态；不得包含 SDL 对象。
         * @thread_safety 只能由拥有此 Context
         * 的渲染主线程调用。

         */
        void updateSurfaceState(const core::SurfaceState& state) noexcept;

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
        /**
         * 关闭命令列表并提交到 graphics queue。
         *
         * 成功返回即是所有跨帧历史的提交边界；后续 present 失败也不得回滚已经提交的 GPU 状态。
         */
        void submitFrameCommands(const VulkanFrame& frame);
        /**
         * 等待本帧 render-finished semaphore 并 present；返回是否已重建交换链。
         *
         * 必须在 `submitFrameCommands()` 成功后调用。present 硬失败可能抛出，但不改变命令已经提交的事实。
         */
        [[nodiscard]] bool presentFrame(const VulkanFrame& frame);
        void cancelFrame(const VulkanFrame& frame);
        void waitIdle() const;

    private:
        struct QueueFamilyIndices {
            std::optional<std::uint32_t> graphics;
            std::optional<std::uint32_t> present;

            [[nodiscard]] bool complete() const noexcept;
        };

        void createDebugMessenger();
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

        [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
        [[nodiscard]] bool deviceExtensionsAvailable(VkPhysicalDevice device) const;
        [[nodiscard]] VulkanRayTracingSupport queryRayTracingSupport(VkPhysicalDevice device) const;
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

        VulkanContextDesc desc_;
        core::SurfaceState surfaceState_;
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
        VulkanRayTracingSupport rayTracingSupport_;
        RayTracingDecision rayTracingDecision_;
        std::vector<const char*> enabledDeviceExtensions_;

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
        bool currentFrameCommandsSubmitted_ = false;
        std::array<VkSemaphore, maxFramesInFlight> imageAvailableSemaphores_{};
        std::vector<VkSemaphore> renderFinishedSemaphores_;
        std::uint32_t currentFrame_ = 0;
        std::uint64_t swapchainGeneration_ = 0;
        std::uint64_t swapchainSurfaceRevision_ = 0;
        bool destroyed_ = false;
    };

} // namespace lumin::render

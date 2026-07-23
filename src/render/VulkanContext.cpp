#include "lumin/render/VulkanContext.hpp"

#include "lumin/platform/Window.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace lumin::render {
    namespace {

        constexpr const char* validationLayers[] = {
            "VK_LAYER_KHRONOS_validation",
        };

        constexpr const char* deviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };

        VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*) {
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                std::cerr << "[vulkan] " << callbackData->pMessage << '\n';
            }

            return VK_FALSE;
        }

        void populateDebugCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
            createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            createInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            createInfo.pfnUserCallback = debugCallback;
        }

        VkResult createDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
                                              const VkAllocationCallbacks* allocator,
                                              VkDebugUtilsMessengerEXT* messenger) {
            const auto function = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));

            if (function == nullptr) {
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }

            return function(instance, createInfo, allocator, messenger);
        }

        void destroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                           const VkAllocationCallbacks* allocator) {
            const auto function = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

            if (function != nullptr) {
                function(instance, messenger, allocator);
            }
        }

        std::uint32_t selectApiVersion() {
            std::uint32_t version = VK_API_VERSION_1_0;
            if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS) {
                version = VK_API_VERSION_1_0;
            }

            if (version >= VK_API_VERSION_1_3) {
                return VK_API_VERSION_1_3;
            }

            if (version >= VK_API_VERSION_1_2) {
                return VK_API_VERSION_1_2;
            }

            if (version >= VK_API_VERSION_1_1) {
                return VK_API_VERSION_1_1;
            }

            return VK_API_VERSION_1_0;
        }

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

        VkImageView createImageView(VkDevice device, VkImage image, VkFormat format) {
            VkImageViewCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.image = image;
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = format;
            info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            info.subresourceRange.baseMipLevel = 0;
            info.subresourceRange.levelCount = 1;
            info.subresourceRange.baseArrayLayer = 0;
            info.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            checkVk(vkCreateImageView(device, &info, nullptr, &view), "Failed to create swapchain image view.");
            return view;
        }

    } // namespace

    bool VulkanContext::QueueFamilyIndices::complete() const noexcept {
        return graphics.has_value() && present.has_value();
    }

    VulkanContext::VulkanContext(platform::Window& window, const VulkanContextDesc& desc)
        : window_(window), desc_(desc) {
        apiVersion_ = selectApiVersion();
        if (apiVersion_ < VK_API_VERSION_1_3) {
            throw std::runtime_error("Lumin Engine requires Vulkan 1.3 for Dynamic Rendering.");
        }
        validationEnabled_ = desc_.enableValidation && validationLayersAvailable();
        debugUtilsEnabled_ = instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        createInstance();
        createDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createDevice();
        loadDebugUtilsFunctions();
        createCommandPool();
        createSwapchainResources();
        createCommandBuffers();
        createSyncObjects();
    }

    VulkanContext::~VulkanContext() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);

            cleanupSwapchainResources();
            for (std::uint32_t index = 0; index < maxFramesInFlight; ++index) {
                if (imageAvailableSemaphores_[index] != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device_, imageAvailableSemaphores_[index], nullptr);
                }
                if (inFlightFences_[index] != VK_NULL_HANDLE) {
                    vkDestroyFence(device_, inFlightFences_[index], nullptr);
                }
            }

            if (commandPool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, commandPool_, nullptr);
            }

            vkDestroyDevice(device_, nullptr);
        }

        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }

        if (debugMessenger_ != VK_NULL_HANDLE) {
            destroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        }

        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    VkInstance VulkanContext::instance() const noexcept {
        return instance_;
    }

    VkPhysicalDevice VulkanContext::physicalDevice() const noexcept {
        return physicalDevice_;
    }

    VkDevice VulkanContext::device() const noexcept {
        return device_;
    }

    VkSurfaceKHR VulkanContext::surface() const noexcept {
        return surface_;
    }

    VkQueue VulkanContext::graphicsQueue() const noexcept {
        return graphicsQueue_;
    }

    VkQueue VulkanContext::presentQueue() const noexcept {
        return presentQueue_;
    }

    VkCommandPool VulkanContext::commandPool() const noexcept {
        return commandPool_;
    }

    std::uint32_t VulkanContext::graphicsQueueFamily() const noexcept {
        return queueFamilies_.graphics.value_or(0);
    }

    std::uint32_t VulkanContext::presentQueueFamily() const noexcept {
        return queueFamilies_.present.value_or(0);
    }

    std::uint32_t VulkanContext::apiVersion() const noexcept {
        return apiVersion_;
    }

    VkSwapchainKHR VulkanContext::swapchain() const noexcept {
        return swapchain_;
    }

    VkFormat VulkanContext::swapchainFormat() const noexcept {
        return swapchainImageFormat_;
    }

    VkExtent2D VulkanContext::swapchainExtent() const noexcept {
        return swapchainExtent_;
    }

    std::uint32_t VulkanContext::swapchainMinImageCount() const noexcept {
        return minImageCount_;
    }

    std::uint32_t VulkanContext::swapchainImageCount() const noexcept {
        return static_cast<std::uint32_t>(swapchainImages_.size());
    }

    const std::vector<VkImage>& VulkanContext::swapchainImages() const noexcept {
        return swapchainImages_;
    }

    const std::vector<VkImageView>& VulkanContext::swapchainImageViews() const noexcept {
        return swapchainImageViews_;
    }

    std::uint64_t VulkanContext::swapchainGeneration() const noexcept {
        return swapchainGeneration_;
    }

    PFN_vkCmdBeginDebugUtilsLabelEXT VulkanContext::cmdBeginDebugUtilsLabel() const noexcept {
        return cmdBeginDebugUtilsLabel_;
    }

    PFN_vkCmdEndDebugUtilsLabelEXT VulkanContext::cmdEndDebugUtilsLabel() const noexcept {
        return cmdEndDebugUtilsLabel_;
    }

    std::optional<VulkanFrame> VulkanContext::beginFrame() {
        checkVk(vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX),
                "Failed to wait for the current frame fence.");

        std::uint32_t imageIndex = 0;
        const VkResult result = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX, imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return std::nullopt;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("Failed to acquire a swapchain image.");
        }

        checkVk(vkResetFences(device_, 1, &inFlightFences_[currentFrame_]), "Failed to reset the frame fence.");
        checkVk(vkResetCommandBuffer(commandBuffers_[currentFrame_], 0), "Failed to reset the frame command buffer.");
        return VulkanFrame{commandBuffers_[currentFrame_], currentFrame_, imageIndex};
    }

    bool VulkanContext::submitFrame(const VulkanFrame& frame) {
        if (frame.frameIndex != currentFrame_) {
            throw std::invalid_argument("VulkanFrame does not belong to the current frame slot.");
        }

        const VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
        const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        const VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[frame.imageIndex]};
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        checkVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]),
                "Failed to submit the Vulkan frame.");

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &frame.imageIndex;
        const VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);
        const bool needsRecreate =
            result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || window_.framebufferResized();
        if (!needsRecreate && result != VK_SUCCESS) {
            throw std::runtime_error("Failed to present the Vulkan frame.");
        }

        currentFrame_ = (currentFrame_ + 1) % maxFramesInFlight;
        if (needsRecreate) {
            recreateSwapchain();
        }
        return needsRecreate;
    }

    void VulkanContext::waitIdle() const {
        if (device_ != VK_NULL_HANDLE) {
            checkVk(vkDeviceWaitIdle(device_), "Failed to wait for the Vulkan device.");
        }
    }

    void VulkanContext::createInstance() {
        VkApplicationInfo appInfo;
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pNext = nullptr;
        appInfo.pApplicationName = desc_.applicationName.c_str();
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Lumin Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = apiVersion_;

        std::vector<const char*> extensions = requiredExtensions();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo;
        populateDebugCreateInfo(debugCreateInfo);

        VkInstanceCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pNext = validationEnabled_ && debugUtilsEnabled_ ? &debugCreateInfo : nullptr;
        createInfo.flags = 0;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        if (validationEnabled_) {
            createInfo.enabledLayerCount = static_cast<std::uint32_t>(std::size(validationLayers));
            createInfo.ppEnabledLayerNames = validationLayers;
        } else {
            createInfo.enabledLayerCount = 0;
            createInfo.ppEnabledLayerNames = nullptr;
        }

        if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan instance.");
        }
    }

    void VulkanContext::createDebugMessenger() {
        if (!validationEnabled_ || !debugUtilsEnabled_) {
            return;
        }

        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        populateDebugCreateInfo(createInfo);

        if (createDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan debug messenger.");
        }
    }

    void VulkanContext::createSurface() {
        surface_ = window_.createSurface(instance_);
    }

    void VulkanContext::pickPhysicalDevice() {
        std::uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

        if (deviceCount == 0) {
            throw std::runtime_error("No Vulkan-capable GPU found.");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

        for (VkPhysicalDevice candidate : devices) {
            if (isDeviceSuitable(candidate)) {
                physicalDevice_ = candidate;
                queueFamilies_ = findQueueFamilies(candidate);
                break;
            }
        }

        if (physicalDevice_ == VK_NULL_HANDLE) {
            throw std::runtime_error("No suitable Vulkan physical device found.");
        }
    }

    void VulkanContext::createDevice() {
        const float queuePriority = 1.0f;
        std::set<std::uint32_t> uniqueQueueFamilies = {
            queueFamilies_.graphics.value(),
            queueFamilies_.present.value(),
        };

        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        queueInfos.reserve(uniqueQueueFamilies.size());

        for (std::uint32_t family : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueInfo;
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.pNext = nullptr;
            queueInfo.flags = 0;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;
            queueInfos.push_back(queueInfo);
        }

        VkPhysicalDeviceFeatures features{};
        features.multiDrawIndirect = VK_TRUE;

        VkPhysicalDeviceVulkan11Features vulkan11Features{};
        vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan11Features.shaderDrawParameters = VK_TRUE;

        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.pNext = &vulkan11Features;
        vulkan12Features.descriptorIndexing = VK_TRUE;
        vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vulkan12Features.runtimeDescriptorArray = VK_TRUE;

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
        dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        dynamicRenderingFeatures.dynamicRendering = VK_TRUE;
        dynamicRenderingFeatures.pNext = &vulkan12Features;

        VkDeviceCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &dynamicRenderingFeatures;
        createInfo.flags = 0;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(std::size(deviceExtensions));
        createInfo.ppEnabledExtensionNames = deviceExtensions;
        createInfo.pEnabledFeatures = &features;

        if (validationEnabled_) {
            createInfo.enabledLayerCount = static_cast<std::uint32_t>(std::size(validationLayers));
            createInfo.ppEnabledLayerNames = validationLayers;
        } else {
            createInfo.enabledLayerCount = 0;
            createInfo.ppEnabledLayerNames = nullptr;
        }

        if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan logical device.");
        }

        vkGetDeviceQueue(device_, queueFamilies_.graphics.value(), 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, queueFamilies_.present.value(), 0, &presentQueue_);
    }

    void VulkanContext::loadDebugUtilsFunctions() {
        if (!debugUtilsEnabled_) {
            return;
        }

        cmdBeginDebugUtilsLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
        cmdEndDebugUtilsLabel_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));

        if (cmdBeginDebugUtilsLabel_ == nullptr || cmdEndDebugUtilsLabel_ == nullptr) {
            cmdBeginDebugUtilsLabel_ = nullptr;
            cmdEndDebugUtilsLabel_ = nullptr;
        }
    }

    void VulkanContext::createCommandPool() {
        VkCommandPoolCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = queueFamilies_.graphics.value();

        if (vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan command pool.");
        }
    }

    void VulkanContext::createSwapchainResources() {
        createSwapchain();
        createImageViews();
        createRenderFinishedSemaphores();
    }

    void VulkanContext::cleanupSwapchainResources() {
        for (VkSemaphore semaphore : renderFinishedSemaphores_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        renderFinishedSemaphores_.clear();
        for (VkImageView view : swapchainImageViews_) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, view, nullptr);
            }
        }
        swapchainImageViews_.clear();
        swapchainImages_.clear();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    void VulkanContext::recreateSwapchain() {
        waitIdle();
        cleanupSwapchainResources();
        createSwapchainResources();
        window_.resetFramebufferResized();
        ++swapchainGeneration_;
    }

    VulkanContext::SwapchainSupport VulkanContext::querySwapchainSupport() const {
        SwapchainSupport support;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &support.capabilities);

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
        support.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, support.formats.data());

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
        support.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount,
                                                  support.presentModes.data());
        return support;
    }

    VkSurfaceFormatKHR VulkanContext::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
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

    VkPresentModeKHR VulkanContext::choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const {
        for (const VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return mode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanContext::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        VkExtent2D extent = window_.framebufferExtent();
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height =
            std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return extent;
    }

    void VulkanContext::createSwapchain() {
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
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const std::uint32_t queueFamilies[] = {graphicsQueueFamily(), presentQueueFamily()};
        if (graphicsQueueFamily() != presentQueueFamily()) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilies;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        checkVk(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_),
                "Failed to create the Vulkan swapchain.");

        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapchainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
        swapchainImageFormat_ = surfaceFormat.format;
        swapchainExtent_ = extent;
    }

    void VulkanContext::createImageViews() {
        swapchainImageViews_.resize(swapchainImages_.size());
        for (std::size_t index = 0; index < swapchainImages_.size(); ++index) {
            swapchainImageViews_[index] = createImageView(device_, swapchainImages_[index], swapchainImageFormat_);
        }
    }

    void VulkanContext::createCommandBuffers() {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = maxFramesInFlight;
        checkVk(vkAllocateCommandBuffers(device_, &allocateInfo, commandBuffers_.data()),
                "Failed to allocate Vulkan command buffers.");
    }

    void VulkanContext::createSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (std::uint32_t index = 0; index < maxFramesInFlight; ++index) {
            checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[index]),
                    "Failed to create image-available semaphore.");
            checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[index]),
                    "Failed to create in-flight fence.");
        }
    }

    void VulkanContext::createRenderFinishedSemaphores() {
        renderFinishedSemaphores_.resize(swapchainImages_.size());
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkSemaphore& semaphore : renderFinishedSemaphores_) {
            checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore),
                    "Failed to create render-finished semaphore.");
        }
    }

    bool VulkanContext::validationLayersAvailable() const {
        std::uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

        for (const char* requiredLayer : validationLayers) {
            const auto found =
                std::find_if(layers.begin(), layers.end(), [requiredLayer](const VkLayerProperties& layer) {
                    return std::strcmp(requiredLayer, layer.layerName) == 0;
                });

            if (found == layers.end()) {
                return false;
            }
        }

        return true;
    }

    bool VulkanContext::instanceExtensionAvailable(const char* extensionName) const {
        std::uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
        return std::any_of(extensions.begin(), extensions.end(), [extensionName](const VkExtensionProperties& item) {
            return std::strcmp(extensionName, item.extensionName) == 0;
        });
    }

    std::vector<const char*> VulkanContext::requiredExtensions() const {
        std::vector<const char*> extensions = window_.requiredInstanceExtensions();

        if (debugUtilsEnabled_ &&
            std::find(extensions.begin(), extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == extensions.end()) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        return extensions;
    }

    VulkanContext::QueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice device) const {
        QueueFamilyIndices indices;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);

        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        for (std::uint32_t familyIndex = 0; familyIndex < familyCount; ++familyIndex) {
            const VkQueueFamilyProperties& family = families[familyIndex];

            if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                indices.graphics = familyIndex;
            }

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, familyIndex, surface_, &presentSupport);
            if (presentSupport == VK_TRUE) {
                indices.present = familyIndex;
            }

            if (indices.complete()) {
                break;
            }
        }

        return indices;
    }

    bool VulkanContext::deviceExtensionsAvailable(VkPhysicalDevice device) const {
        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        for (const char* requiredExtension : deviceExtensions) {
            const auto found = std::find_if(availableExtensions.begin(), availableExtensions.end(),
                                            [requiredExtension](const VkExtensionProperties& extension) {
                                                return std::strcmp(requiredExtension, extension.extensionName) == 0;
                                            });

            if (found == availableExtensions.end()) {
                return false;
            }
        }

        return true;
    }

    bool VulkanContext::isDeviceSuitable(VkPhysicalDevice device) const {
        VkPhysicalDeviceVulkan11Features vulkan11Features{};
        vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.pNext = &vulkan11Features;

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
        dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        dynamicRenderingFeatures.pNext = &vulkan12Features;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &dynamicRenderingFeatures;
        vkGetPhysicalDeviceFeatures2(device, &features);

        return findQueueFamilies(device).complete() && deviceExtensionsAvailable(device) &&
               features.features.multiDrawIndirect == VK_TRUE && vulkan11Features.shaderDrawParameters == VK_TRUE &&
               vulkan12Features.descriptorIndexing == VK_TRUE &&
               vulkan12Features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
               vulkan12Features.runtimeDescriptorArray == VK_TRUE &&
               dynamicRenderingFeatures.dynamicRendering == VK_TRUE;
    }

} // namespace lumin::render

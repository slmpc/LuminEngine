#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#include "render/platform/vulkan/VulkanContext.hpp"

#include "render/platform/Window.hpp"
#include "render/BackendLifetime.hpp"
#include "render/ModelRenderer.hpp"
#include "render/RayTracingBuildConfiguration.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace lumin::render {
    namespace {

        constexpr const char* validationLayers[] = {
            "VK_LAYER_KHRONOS_validation",
        };

        constexpr const char* baseDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };

        class NvrhiMessageCallback final : public nvrhi::IMessageCallback {
        public:
            void message(nvrhi::MessageSeverity severity, const char* messageText) override {
                std::cerr << "[nvrhi:" << static_cast<int>(severity) << "] " << messageText << '\n';
            }
        };

        NvrhiMessageCallback nvrhiMessageCallback;

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
        try {
            VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
            apiVersion_ = selectApiVersion();
            if (apiVersion_ < VK_API_VERSION_1_3) {
                throw std::runtime_error("Lumin Engine requires Vulkan 1.3 for Dynamic Rendering.");
            }
            validationEnabled_ = desc_.enableValidation && validationLayersAvailable();
            debugUtilsEnabled_ = instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

            createInstance();
            VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));
            createDebugMessenger();
            createSurface();
            pickPhysicalDevice();
            createDevice();
            VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device_));
            createRhiDevice();
            loadDebugUtilsFunctions();
            createSwapchainResources();
            createFrameResources();
        } catch (...) {
            destroy();
            throw;
        }
    }

    VulkanContext::~VulkanContext() {
        destroy();
    }

    void VulkanContext::destroy() noexcept {
        detail::destroyBackendLifetime(
            destroyed_,
            detail::BackendLifetimeAvailability{
                .children = true,
                .swapchainWrappers = device_ != VK_NULL_HANDLE || !swapchainTextures_.empty(),
                .rhiDevice = static_cast<bool>(rhiDevice_),
                .vulkanDevice = device_ != VK_NULL_HANDLE,
            },
            [this] {
                if (rhiDevice_) {
                    rhiDevice_->waitForIdle();
                }
                for (std::uint32_t index = 0; index < maxFramesInFlight; ++index) {
                    commandLists_[index] = nullptr;
                    frameQueries_[index] = nullptr;
                }
            },
            [this] {
                if (device_ != VK_NULL_HANDLE) {
                    cleanupSwapchainResources();
                    for (std::uint32_t index = 0; index < maxFramesInFlight; ++index) {
                        if (imageAvailableSemaphores_[index] != VK_NULL_HANDLE) {
                            vkDestroySemaphore(device_, imageAvailableSemaphores_[index], nullptr);
                            imageAvailableSemaphores_[index] = VK_NULL_HANDLE;
                        }
                    }
                } else {
                    swapchainTextures_.clear();
                    swapchainTextureInitialized_.clear();
                }
            },
            [this] {
                rhiDevice_ = nullptr;
            },
            [this] {
                if (device_ != VK_NULL_HANDLE) {
                    vkDestroyDevice(device_, nullptr);
                    device_ = VK_NULL_HANDLE;
                }
            });

        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }

        if (debugMessenger_ != VK_NULL_HANDLE) {
            destroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
            debugMessenger_ = VK_NULL_HANDLE;
        }

        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        physicalDevice_ = VK_NULL_HANDLE;
        graphicsQueue_ = VK_NULL_HANDLE;
        presentQueue_ = VK_NULL_HANDLE;
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

    std::uint32_t VulkanContext::swapchainWidth() const noexcept {
        return swapchainExtent_.width;
    }

    std::uint32_t VulkanContext::swapchainHeight() const noexcept {
        return swapchainExtent_.height;
    }

    nvrhi::Format VulkanContext::swapchainRhiFormat() const noexcept {
        return mapSwapchainFormat(swapchainImageFormat_);
    }

    bool VulkanContext::swapchainIsSrgb() const noexcept {
        return swapchainImageFormat_ == VK_FORMAT_R8G8B8A8_SRGB || swapchainImageFormat_ == VK_FORMAT_B8G8R8A8_SRGB ||
               swapchainImageFormat_ == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
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

    const nvrhi::vulkan::DeviceHandle& VulkanContext::rhiDevice() const noexcept {
        return rhiDevice_;
    }

    const std::vector<nvrhi::TextureHandle>& VulkanContext::swapchainTextures() const noexcept {
        return swapchainTextures_;
    }

    nvrhi::ResourceStates VulkanContext::swapchainTextureInitialState(std::uint32_t imageIndex) const {
        if (imageIndex >= swapchainTextureInitialized_.size()) {
            throw std::out_of_range("Swapchain image index is out of range.");
        }
        return swapchainTextureInitialized_[imageIndex] ? nvrhi::ResourceStates::Present
                                                        : nvrhi::ResourceStates::Unknown;
    }

    ModelRendererCapabilities VulkanContext::modelRendererCapabilities() const noexcept {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        return ModelRendererCapabilities{
            .maxMaterialTextureArrayLength = properties.limits.maxDescriptorSetSampledImages,
            .maxDrawIndirectCount = properties.limits.maxDrawIndirectCount,
            .maxImageDimension2D = properties.limits.maxImageDimension2D,
        };
    }

    const VulkanRayTracingSupport& VulkanContext::rayTracingSupport() const noexcept {
        return rayTracingSupport_;
    }

    const RayTracingDecision& VulkanContext::rayTracingDecision() const noexcept {
        return rayTracingDecision_;
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
        if (frameQueryPending_[currentFrame_]) {
            if (!rhiDevice_->pollEventQuery(frameQueries_[currentFrame_])) {
                rhiDevice_->waitEventQuery(frameQueries_[currentFrame_]);
            }
            rhiDevice_->resetEventQuery(frameQueries_[currentFrame_]);
            frameQueryPending_[currentFrame_] = false;
        }

        // NvRHI 会让已提交 command buffer 强引用本帧使用过的 framebuffer、binding set 和资源。
        // 帧槽完成后必须主动退休这些 command buffer，否则每帧临时对象会永久滞留在驱动/CPU 堆中。
        rhiDevice_->runGarbageCollection();

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

        commandLists_[currentFrame_]->open();
        commandLists_[currentFrame_]->setEnableAutomaticBarriers(false);
        return VulkanFrame{commandLists_[currentFrame_], currentFrame_, imageIndex};
    }

    void VulkanContext::submitFrameCommands(const VulkanFrame& frame) {
        if (frame.frameIndex != currentFrame_) {
            throw std::invalid_argument("VulkanFrame does not belong to the current frame slot.");
        }
        if (frame.commandList.Get() != commandLists_[currentFrame_].Get()) {
            throw std::invalid_argument("VulkanFrame command list does not belong to the current frame slot.");
        }
        if (currentFrameCommandsSubmitted_) {
            throw std::logic_error("The current Vulkan frame was already submitted.");
        }

        frame.commandList->close();
        const VkSemaphore renderFinished = renderFinishedSemaphores_[frame.imageIndex];
        rhiDevice_->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, imageAvailableSemaphores_[currentFrame_], 0);
        rhiDevice_->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, renderFinished, 0);
        nvrhi::ICommandList* commandLists[] = {frame.commandList};
        rhiDevice_->executeCommandLists(commandLists, std::size(commandLists), nvrhi::CommandQueue::Graphics);
        rhiDevice_->setEventQuery(frameQueries_[currentFrame_], nvrhi::CommandQueue::Graphics);
        frameQueryPending_[currentFrame_] = true;
        currentFrameCommandsSubmitted_ = true;
    }

    bool VulkanContext::presentFrame(const VulkanFrame& frame) {
        if (frame.frameIndex != currentFrame_ || frame.commandList.Get() != commandLists_[currentFrame_].Get()) {
            throw std::invalid_argument("VulkanFrame does not belong to the current frame slot.");
        }
        if (!currentFrameCommandsSubmitted_) {
            throw std::logic_error("A Vulkan frame must be submitted before it can be presented.");
        }

        const VkSemaphore renderFinished = renderFinishedSemaphores_[frame.imageIndex];
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &frame.imageIndex;
        const VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);
        const bool needsRecreate =
            result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || window_.framebufferResized();
        currentFrameCommandsSubmitted_ = false;
        currentFrame_ = (currentFrame_ + 1) % maxFramesInFlight;
        if (!needsRecreate && result != VK_SUCCESS) {
            throw std::runtime_error("Failed to present the Vulkan frame.");
        }

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            swapchainTextureInitialized_[frame.imageIndex] = true;
        }

        if (needsRecreate) {
            recreateSwapchain();
        }
        return needsRecreate;
    }

    void VulkanContext::cancelFrame(const VulkanFrame& frame) {
        if (frame.frameIndex != currentFrame_ || frame.commandList.Get() != commandLists_[currentFrame_].Get()) {
            throw std::invalid_argument("VulkanFrame does not belong to the current frame slot.");
        }
        if (currentFrameCommandsSubmitted_) {
            throw std::logic_error("Submitted Vulkan commands cannot be cancelled.");
        }

        frame.commandList->close();
        nvrhi::CommandListHandle cancellation = rhiDevice_->createCommandList();
        if (!cancellation) {
            throw std::runtime_error("Failed to create an NvRHI frame-cancellation command list.");
        }
        cancellation->open();
        cancellation->setEnableAutomaticBarriers(false);
        cancellation->close();
        rhiDevice_->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, imageAvailableSemaphores_[currentFrame_], 0);
        nvrhi::ICommandList* cancellationLists[] = {cancellation};
        rhiDevice_->executeCommandLists(cancellationLists, std::size(cancellationLists), nvrhi::CommandQueue::Graphics);
        waitIdle();
        recreateSwapchain();
    }

    void VulkanContext::waitIdle() const {
        if (rhiDevice_ && !rhiDevice_->waitForIdle()) {
            throw std::runtime_error("Failed to wait for the NvRHI device.");
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

        VkPhysicalDevice fallbackDevice = VK_NULL_HANDLE;
        QueueFamilyIndices fallbackQueueFamilies;
        VulkanRayTracingSupport fallbackSupport;
        RayTracingDecision fallbackDecision;
        std::ostringstream rejectedDiagnostics;

        for (VkPhysicalDevice candidate : devices) {
            if (!isDeviceSuitable(candidate)) {
                continue;
            }

            // 构建策略属于产物 ABI，不能由运行时描述覆盖。OFF 产物也不会触碰任何 RT 专属 Feature 查询。
            RayTracingPolicy policy = desc_.rayTracing;
            policy.build = configuredRayTracingBuildMode;
            VulkanRayTracingSupport support =
                rayTracingImplementationAvailable ? queryRayTracingSupport(candidate) : VulkanRayTracingSupport{};
            RayTracingDecision decision = resolveRayTracingPolicy(policy, rayTracingImplementationAvailable, support);
            if (decision.rejected()) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);
                rejectedDiagnostics << properties.deviceName << ": " << formatRayTracingDecision(decision) << ' ';
                continue;
            }

            if (decision.enabled() || decision.status == RayTracingDecisionStatus::Disabled) {
                physicalDevice_ = candidate;
                queueFamilies_ = findQueueFamilies(candidate);
                rayTracingSupport_ = std::move(support);
                rayTracingDecision_ = std::move(decision);
                break;
            }

            // AUTO fallback 先保留首个 raster 候选，同时继续寻找支持完整 RT pipeline 的设备。
            if (fallbackDevice == VK_NULL_HANDLE) {
                fallbackDevice = candidate;
                fallbackQueueFamilies = findQueueFamilies(candidate);
                fallbackSupport = std::move(support);
                fallbackDecision = std::move(decision);
            }
        }

        if (physicalDevice_ == VK_NULL_HANDLE && fallbackDevice != VK_NULL_HANDLE) {
            physicalDevice_ = fallbackDevice;
            queueFamilies_ = fallbackQueueFamilies;
            rayTracingSupport_ = std::move(fallbackSupport);
            rayTracingDecision_ = std::move(fallbackDecision);
        }

        if (physicalDevice_ == VK_NULL_HANDLE) {
            std::string message = "No suitable Vulkan physical device found.";
            if (!rejectedDiagnostics.str().empty()) {
                message += " Ray tracing diagnostics: " + rejectedDiagnostics.str();
            }
            throw std::runtime_error(message);
        }

        enabledDeviceExtensions_.assign(std::begin(baseDeviceExtensions), std::end(baseDeviceExtensions));
        for (const std::string_view extension : enabledVulkanRayTracingDeviceExtensions(rayTracingDecision_)) {
            enabledDeviceExtensions_.push_back(extension.data());
        }

        if (rayTracingDecision_.enabled() || rayTracingDecision_.status == RayTracingDecisionStatus::Fallback) {
            std::cerr << "[vulkan] " << formatRayTracingDecision(rayTracingDecision_) << '\n';
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
        features.shaderInt64 =
            rayTracingDecision_.enabled() && rayTracingSupport_.supportsSharcShaderStorage() ? VK_TRUE : VK_FALSE;

        VkPhysicalDeviceVulkan11Features vulkan11Features{};
        vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan11Features.shaderDrawParameters = VK_TRUE;
        vulkan11Features.uniformAndStorageBuffer16BitAccess =
            rayTracingDecision_.enabled() && rayTracingSupport_.supportsSharcShaderStorage() ? VK_TRUE : VK_FALSE;
        vulkan11Features.storageBuffer16BitAccess =
            rayTracingDecision_.enabled() && rayTracingSupport_.supportsSharcShaderStorage() ? VK_TRUE : VK_FALSE;

        VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR computeShaderDerivativesFeatures{};
        computeShaderDerivativesFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR;

        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};
        rayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
        accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.pNext = &vulkan11Features;
        vulkan12Features.descriptorIndexing = VK_TRUE;
        vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vulkan12Features.runtimeDescriptorArray = VK_TRUE;
        vulkan12Features.timelineSemaphore = VK_TRUE;
        vulkan12Features.shaderFloat16 =
            rayTracingDecision_.enabled() && rayTracingSupport_.supportsSharcShaderStorage() ? VK_TRUE : VK_FALSE;

        if (rayTracingDecision_.enabled()) {
            vulkan12Features.bufferDeviceAddress = VK_TRUE;
            vulkan12Features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
            accelerationStructureFeatures.accelerationStructure = VK_TRUE;
            accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;
            rayTracingPipelineFeatures.rayTracingPipeline = VK_TRUE;
            rayTracingPipelineFeatures.pNext = rayTracingDecision_.enablesRayQuery() ? &rayQueryFeatures : nullptr;
            rayQueryFeatures.rayQuery = rayTracingDecision_.enablesRayQuery() ? VK_TRUE : VK_FALSE;
            computeShaderDerivativesFeatures.computeDerivativeGroupQuads = VK_TRUE;
            if (rayTracingDecision_.enablesRayQuery()) {
                rayQueryFeatures.pNext = &computeShaderDerivativesFeatures;
            } else {
                rayTracingPipelineFeatures.pNext = &computeShaderDerivativesFeatures;
            }
            vulkan11Features.pNext = &accelerationStructureFeatures;
        }

        VkPhysicalDeviceVulkan13Features vulkan13Features{};
        vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13Features.dynamicRendering = VK_TRUE;
        vulkan13Features.synchronization2 = VK_TRUE;
        vulkan13Features.pNext = &vulkan12Features;

        VkDeviceCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &vulkan13Features;
        createInfo.flags = 0;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabledDeviceExtensions_.size());
        createInfo.ppEnabledExtensionNames = enabledDeviceExtensions_.data();
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

    void VulkanContext::createRhiDevice() {
        nvrhi::vulkan::DeviceDesc deviceDesc{};
        deviceDesc.errorCB = &nvrhiMessageCallback;
        deviceDesc.instance = instance_;
        deviceDesc.physicalDevice = physicalDevice_;
        deviceDesc.device = device_;
        deviceDesc.graphicsQueue = graphicsQueue_;
        deviceDesc.graphicsQueueIndex = static_cast<int>(graphicsQueueFamily());
        deviceDesc.deviceExtensions = enabledDeviceExtensions_.data();
        deviceDesc.numDeviceExtensions = enabledDeviceExtensions_.size();
        deviceDesc.bufferDeviceAddressSupported = rayTracingDecision_.enabled();
        rhiDevice_ = nvrhi::vulkan::createDevice(deviceDesc);
        if (!rhiDevice_) {
            throw std::runtime_error("Failed to create the NvRHI Vulkan device.");
        }
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

    void VulkanContext::createSwapchainResources() {
        createSwapchain();
        createImageViews();
        createSwapchainTextures();
        createRenderFinishedSemaphores();
    }

    void VulkanContext::cleanupSwapchainResources() {
        swapchainTextures_.clear();
        swapchainTextureInitialized_.clear();
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

    void VulkanContext::createSwapchainTextures() {
        const nvrhi::Format format = mapSwapchainFormat(swapchainImageFormat_);
        if (format == nvrhi::Format::UNKNOWN) {
            throw std::runtime_error("NvRHI does not support the selected Vulkan swapchain format.");
        }

        swapchainTextures_.reserve(swapchainImages_.size());
        swapchainTextureInitialized_.assign(swapchainImages_.size(), false);
        for (std::size_t index = 0; index < swapchainImages_.size(); ++index) {
            nvrhi::TextureDesc textureDesc{};
            textureDesc.width = swapchainExtent_.width;
            textureDesc.height = swapchainExtent_.height;
            textureDesc.format = format;
            textureDesc.debugName = "Swapchain image " + std::to_string(index);
            textureDesc.isShaderResource = false;
            textureDesc.isRenderTarget = true;
            textureDesc.initialState = nvrhi::ResourceStates::Unknown;
            nvrhi::TextureHandle texture = rhiDevice_->createHandleForNativeTexture(
                nvrhi::ObjectTypes::VK_Image, nvrhi::Object(swapchainImages_[index]), textureDesc);
            if (!texture) {
                throw std::runtime_error("Failed to wrap a Vulkan swapchain image with NvRHI.");
            }
            swapchainTextures_.push_back(std::move(texture));
        }
    }

    void VulkanContext::createFrameResources() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (std::uint32_t index = 0; index < maxFramesInFlight; ++index) {
            checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[index]),
                    "Failed to create image-available semaphore.");
            commandLists_[index] = rhiDevice_->createCommandList();
            frameQueries_[index] = rhiDevice_->createEventQuery();
            if (!commandLists_[index] || !frameQueries_[index]) {
                throw std::runtime_error("Failed to create NvRHI frame resources.");
            }
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

        for (const char* requiredExtension : baseDeviceExtensions) {
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

    VulkanRayTracingSupport VulkanContext::queryRayTracingSupport(VkPhysicalDevice device) const {
        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::vector<std::string_view> extensionNames;
        extensionNames.reserve(availableExtensions.size());
        for (const VkExtensionProperties& extension : availableExtensions) {
            extensionNames.emplace_back(extension.extensionName);
        }
        const VulkanRayTracingExtensionSupport extensionSupport = inspectVulkanRayTracingExtensions(extensionNames);

        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

        VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR computeShaderDerivativesFeatures{};
        computeShaderDerivativesFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};
        rayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
        accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

        void* extensionFeatureChain = nullptr;
        if (extensionSupport.rayQuery) {
            rayQueryFeatures.pNext = extensionFeatureChain;
            extensionFeatureChain = &rayQueryFeatures;
        }
        if (extensionSupport.rayTracingPipeline) {
            rayTracingPipelineFeatures.pNext = extensionFeatureChain;
            extensionFeatureChain = &rayTracingPipelineFeatures;
        }
        if (extensionSupport.accelerationStructure) {
            accelerationStructureFeatures.pNext = extensionFeatureChain;
            extensionFeatureChain = &accelerationStructureFeatures;
        }
        if (extensionSupport.computeShaderDerivatives) {
            computeShaderDerivativesFeatures.pNext = extensionFeatureChain;
            extensionFeatureChain = &computeShaderDerivativesFeatures;
        }

        VkPhysicalDeviceVulkan11Features vulkan11Features{};
        vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan11Features.pNext = extensionFeatureChain;

        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.pNext = &vulkan11Features;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &vulkan12Features;
        vkGetPhysicalDeviceFeatures2(device, &features);

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties{};
        rayTracingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        if (extensionSupport.rayTracingPipeline) {
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &rayTracingProperties;
            vkGetPhysicalDeviceProperties2(device, &properties);
        }

        return evaluateVulkanRayTracingSupport(VulkanRayTracingDeviceProbe{
            .extensions = extensionSupport,
            .features =
                {
                    .shaderInt64 = features.features.shaderInt64 == VK_TRUE,
                    .shaderFloat16 = vulkan12Features.shaderFloat16 == VK_TRUE,
                    .uniformAndStorageBuffer16BitAccess =
                        vulkan11Features.uniformAndStorageBuffer16BitAccess == VK_TRUE,
                    .storageBuffer16BitAccess = vulkan11Features.storageBuffer16BitAccess == VK_TRUE,
                    .bufferDeviceAddress = vulkan12Features.bufferDeviceAddress == VK_TRUE,
                    .shaderStorageBufferArrayNonUniformIndexing =
                        vulkan12Features.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE,
                    .descriptorBindingPartiallyBound = vulkan12Features.descriptorBindingPartiallyBound == VK_TRUE,
                    .accelerationStructure = accelerationStructureFeatures.accelerationStructure == VK_TRUE,
                    .rayTracingPipeline = rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE,
                    .rayQuery = rayQueryFeatures.rayQuery == VK_TRUE,
                    .computeDerivativeGroupQuads =
                        computeShaderDerivativesFeatures.computeDerivativeGroupQuads == VK_TRUE,
                },
            .maxRayRecursionDepth = rayTracingProperties.maxRayRecursionDepth,
        });
    }

    bool VulkanContext::isDeviceSuitable(VkPhysicalDevice device) const {
        VkPhysicalDeviceVulkan11Features vulkan11Features{};
        vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.pNext = &vulkan11Features;

        VkPhysicalDeviceVulkan13Features vulkan13Features{};
        vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13Features.pNext = &vulkan12Features;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &vulkan13Features;
        vkGetPhysicalDeviceFeatures2(device, &features);

        if (vulkan13Features.synchronization2 != VK_TRUE) {
            std::cerr << "[vulkan] Rejecting Vulkan physical device without synchronization2 support.\n";
        }

        return findQueueFamilies(device).complete() && deviceExtensionsAvailable(device) &&
               features.features.multiDrawIndirect == VK_TRUE && vulkan11Features.shaderDrawParameters == VK_TRUE &&
               vulkan12Features.descriptorIndexing == VK_TRUE &&
               vulkan12Features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
               vulkan12Features.runtimeDescriptorArray == VK_TRUE && vulkan12Features.timelineSemaphore == VK_TRUE &&
               vulkan13Features.dynamicRendering == VK_TRUE && vulkan13Features.synchronization2 == VK_TRUE;
    }

} // namespace lumin::render

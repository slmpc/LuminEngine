#include "lumin/render/VulkanContext.hpp"

#include "lumin/platform/Window.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
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
            if (vkEnumerateInstanceVersion != nullptr) {
                vkEnumerateInstanceVersion(&version);
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

        createInstance();
        createDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createDevice();
        createCommandPool();
    }

    VulkanContext::~VulkanContext() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);

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
        createInfo.pNext = validationEnabled_ ? &debugCreateInfo : nullptr;
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
        if (!validationEnabled_) {
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

        VkPhysicalDeviceFeatures features;
        features = {};

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
        dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        dynamicRenderingFeatures.dynamicRendering = VK_TRUE;

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

    std::vector<const char*> VulkanContext::requiredExtensions() const {
        std::vector<const char*> extensions = window_.requiredInstanceExtensions();

        if (validationEnabled_) {
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
        VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
        dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &dynamicRenderingFeatures;
        vkGetPhysicalDeviceFeatures2(device, &features);

        return findQueueFamilies(device).complete() && deviceExtensionsAvailable(device) &&
               dynamicRenderingFeatures.dynamicRendering == VK_TRUE;
    }

} // namespace lumin::render

#include "render/platform/vulkan/VulkanRayTracingCapabilities.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#if !defined(LUMIN_CTEST_SKIP_RETURN_CODE)
#define LUMIN_CTEST_SKIP_RETURN_CODE 77
#endif

namespace {

    class VulkanInstance final {
    public:
        explicit VulkanInstance(VkInstance instance) noexcept : instance_(instance) {
        }

        ~VulkanInstance() {
            if (instance_ != VK_NULL_HANDLE) {
                vkDestroyInstance(instance_, nullptr);
            }
        }

        VulkanInstance(const VulkanInstance&) = delete;
        VulkanInstance& operator=(const VulkanInstance&) = delete;

        [[nodiscard]] VkInstance get() const noexcept {
            return instance_;
        }

    private:
        VkInstance instance_ = VK_NULL_HANDLE;
    };

    [[nodiscard]] int skip(const char* reason) {
        std::printf("SKIP: %s\n", reason);
        return LUMIN_CTEST_SKIP_RETURN_CODE;
    }

    [[nodiscard]] std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device) {
        std::uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
            return {};
        }
        std::vector<VkExtensionProperties> result(count);
        if (count != 0 && vkEnumerateDeviceExtensionProperties(device, nullptr, &count, result.data()) != VK_SUCCESS) {
            return {};
        }
        result.resize(count);
        return result;
    }

    [[nodiscard]] lumin::render::VulkanRayTracingDeviceProbe rayTracingProbe(VkPhysicalDevice device) {
        const std::vector<VkExtensionProperties> properties = deviceExtensions(device);
        std::vector<std::string_view> extensionNames;
        extensionNames.reserve(properties.size());
        for (const VkExtensionProperties& property : properties) {
            extensionNames.emplace_back(property.extensionName);
        }

        const lumin::render::VulkanRayTracingExtensionSupport extensionSupport =
            lumin::render::inspectVulkanRayTracingExtensions(extensionNames);

        VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR computeDerivatives{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan11Features vulkan11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &vulkan11;
        vulkan11.pNext = &vulkan12;
        void** featureTail = &vulkan12.pNext;
        if (extensionSupport.accelerationStructure) {
            *featureTail = &acceleration;
            featureTail = &acceleration.pNext;
        }
        if (extensionSupport.rayTracingPipeline) {
            *featureTail = &rayTracing;
            featureTail = &rayTracing.pNext;
        }
        if (extensionSupport.rayQuery) {
            *featureTail = &rayQuery;
            featureTail = &rayQuery.pNext;
        }
        if (extensionSupport.computeShaderDerivatives) {
            *featureTail = &computeDerivatives;
            featureTail = &computeDerivatives.pNext;
        }
        *featureTail = nullptr;
        vkGetPhysicalDeviceFeatures2(device, &features);

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 deviceProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        deviceProperties.pNext = extensionSupport.rayTracingPipeline ? &rayTracingProperties : nullptr;
        vkGetPhysicalDeviceProperties2(device, &deviceProperties);

        return lumin::render::VulkanRayTracingDeviceProbe{
            .extensions = extensionSupport,
            .features =
                {
                    .shaderInt64 = features.features.shaderInt64 == VK_TRUE,
                    .shaderFloat16 = vulkan12.shaderFloat16 == VK_TRUE,
                    .uniformAndStorageBuffer16BitAccess = vulkan11.uniformAndStorageBuffer16BitAccess == VK_TRUE,
                    .storageBuffer16BitAccess = vulkan11.storageBuffer16BitAccess == VK_TRUE,
                    .bufferDeviceAddress = vulkan12.bufferDeviceAddress == VK_TRUE,
                    .shaderStorageBufferArrayNonUniformIndexing =
                        vulkan12.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE,
                    .descriptorBindingPartiallyBound = vulkan12.descriptorBindingPartiallyBound == VK_TRUE,
                    .accelerationStructure = acceleration.accelerationStructure == VK_TRUE,
                    .rayTracingPipeline = rayTracing.rayTracingPipeline == VK_TRUE,
                    .rayQuery = rayQuery.rayQuery == VK_TRUE,
                    .computeDerivativeGroupQuads = computeDerivatives.computeDerivativeGroupQuads == VK_TRUE,
                },
            .maxRayRecursionDepth = rayTracingProperties.maxRayRecursionDepth,
        };
    }

    [[nodiscard]] bool isVulkan13Device(VkPhysicalDevice device) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        return VK_API_VERSION_MAJOR(properties.apiVersion) > 1 ||
               (VK_API_VERSION_MAJOR(properties.apiVersion) == 1 && VK_API_VERSION_MINOR(properties.apiVersion) >= 3);
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Expected one probe mode: --vulkan13, --raytracing, or --sharc.\n");
        return 2;
    }

    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS || loaderVersion < VK_API_VERSION_1_3) {
        return skip("Vulkan 1.3 loader is unavailable");
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "LuminEngine hardware probe";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    application.pEngineName = "LuminEngine";
    application.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &application;
    VkInstance rawInstance = VK_NULL_HANDLE;
    if (vkCreateInstance(&createInfo, nullptr, &rawInstance) != VK_SUCCESS) {
        return skip("Vulkan 1.3 instance creation is unavailable");
    }
    const VulkanInstance instance(rawInstance);

    std::uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance.get(), &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        return skip("no Vulkan physical device is available");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (vkEnumeratePhysicalDevices(instance.get(), &deviceCount, devices.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to enumerate Vulkan physical devices.\n");
        return 1;
    }
    devices.resize(deviceCount);

    const std::string_view mode = argv[1];
    if (mode == "--vulkan13") {
        for (VkPhysicalDevice device : devices) {
            if (isVulkan13Device(device)) {
                std::puts("Vulkan 1.3 hardware probe passed.");
                return 0;
            }
        }
        return skip("no Vulkan 1.3 physical device is available");
    }

    if (mode != "--raytracing" && mode != "--sharc") {
        std::fprintf(stderr, "Unknown hardware probe mode: %s\n", argv[1]);
        return 2;
    }

    for (VkPhysicalDevice device : devices) {
        if (!isVulkan13Device(device)) {
            continue;
        }
        const lumin::render::VulkanRayTracingSupport support =
            lumin::render::evaluateVulkanRayTracingSupport(rayTracingProbe(device));
        if (support.supportsRayTracingPipeline() && (mode == "--raytracing" || support.supportsSharcShaderStorage())) {
            std::printf("%s hardware probe passed (%s).\n", mode == "--sharc" ? "SHARC capability" : "Ray tracing",
                        lumin::render::vulkanRayTracingTierName(support.tier).data());
            return 0;
        }
    }

    return skip(mode == "--sharc" ? "no device supports RT plus SHARC storage features"
                                  : "no device supports the Vulkan ray tracing pipeline");
}

#include "render/platform/vulkan/VulkanContext.hpp"

#include <concepts>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>

namespace {

    static_assert(std::same_as<decltype(lumin::render::VulkanFrame::commandList), nvrhi::CommandListHandle>);

    static_assert(requires(const lumin::render::VulkanContext& context) {
        { context.rhiDevice() } -> std::same_as<const nvrhi::vulkan::DeviceHandle&>;
        { context.swapchainTextures() } -> std::same_as<const std::vector<nvrhi::TextureHandle>&>;
    });

    static_assert(lumin::render::VulkanContext::mapSwapchainFormat(VK_FORMAT_B8G8R8A8_SRGB) ==
                  nvrhi::Format::SBGRA8_UNORM);
    static_assert(lumin::render::VulkanContext::mapSwapchainFormat(VK_FORMAT_B8G8R8A8_UNORM) ==
                  nvrhi::Format::BGRA8_UNORM);
    static_assert(lumin::render::VulkanContext::mapSwapchainFormat(VK_FORMAT_R8G8B8A8_UNORM) == nvrhi::Format::UNKNOWN);

    bool timelineSemaphorePolicyIsDeclared() {
        const std::filesystem::path sourcePath =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "render/platform/vulkan/VulkanContext.cpp";
        std::ifstream source(sourcePath);
        const std::string contents{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        return contents.find("vulkan12Features.timelineSemaphore = VK_TRUE;") != std::string::npos &&
               contents.find("vulkan12Features.timelineSemaphore == VK_TRUE") != std::string::npos;
    }

    bool synchronization2PolicyIsDeclared() {
        const std::filesystem::path sourcePath =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "render/platform/vulkan/VulkanContext.cpp";
        std::ifstream source(sourcePath);
        const std::string contents{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        const bool emitsSynchronization2Rejection =
            contents.find("Rejecting Vulkan physical device without synchronization2 support.") != std::string::npos;

        return contents.find("VkPhysicalDeviceDynamicRenderingFeatures") == std::string::npos &&
               contents.find("VkPhysicalDeviceVulkan13Features vulkan13Features{};") != std::string::npos &&
               contents.find("vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;") !=
                   std::string::npos &&
               contents.find("vulkan13Features.dynamicRendering = VK_TRUE;") != std::string::npos &&
               contents.find("vulkan13Features.synchronization2 = VK_TRUE;") != std::string::npos &&
               contents.find("vulkan13Features.pNext = &vulkan12Features;") != std::string::npos &&
               contents.find("createInfo.pNext = &vulkan13Features;") != std::string::npos &&
               contents.find("features.pNext = &vulkan13Features;") != std::string::npos &&
               contents.find("vulkan13Features.synchronization2 == VK_TRUE") != std::string::npos &&
               emitsSynchronization2Rejection;
    }

} // namespace

int main() {
    return timelineSemaphorePolicyIsDeclared() && synchronization2PolicyIsDeclared() ? 0 : 1;
}

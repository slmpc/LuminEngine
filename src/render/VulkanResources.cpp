#include "lumin/render/VulkanResources.hpp"

#include "lumin/render/VulkanContext.hpp"

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace lumin::render {
    namespace {

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

    } // namespace

    VulkanResourceManager::VulkanResourceManager(VulkanContext& context) : context_(context) {
    }

    VulkanBuffer VulkanResourceManager::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                     VkMemoryPropertyFlags properties) const {
        VulkanBuffer buffer;
        buffer.size = size;

        VkBufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        createInfo.size = size;
        createInfo.usage = usage;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        checkVk(vkCreateBuffer(context_.device(), &createInfo, nullptr, &buffer.buffer), "Failed to create buffer.");

        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(context_.device(), buffer.buffer, &memoryRequirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, properties);

        checkVk(vkAllocateMemory(context_.device(), &allocateInfo, nullptr, &buffer.memory),
                "Failed to allocate buffer memory.");
        vkBindBufferMemory(context_.device(), buffer.buffer, buffer.memory, 0);
        return buffer;
    }

    void VulkanResourceManager::destroyBuffer(VulkanBuffer& buffer) const {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(context_.device(), buffer.buffer, nullptr);
            buffer.buffer = VK_NULL_HANDLE;
        }

        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(context_.device(), buffer.memory, nullptr);
            buffer.memory = VK_NULL_HANDLE;
        }

        buffer.size = 0;
    }

    void VulkanResourceManager::writeBuffer(const VulkanBuffer& buffer, const void* data, VkDeviceSize size) const {
        void* mapped = nullptr;
        checkVk(vkMapMemory(context_.device(), buffer.memory, 0, size, 0, &mapped), "Failed to map buffer memory.");
        std::memcpy(mapped, data, static_cast<std::size_t>(size));
        vkUnmapMemory(context_.device(), buffer.memory);
    }

    VulkanImage VulkanResourceManager::createImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                                                   VkImageUsageFlags usage, VkImageAspectFlags aspectMask) const {
        VulkanImage image;
        image.format = format;
        image.aspectMask = aspectMask;

        VkImageCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.extent.width = width;
        createInfo.extent.height = height;
        createInfo.extent.depth = 1;
        createInfo.mipLevels = 1;
        createInfo.arrayLayers = 1;
        createInfo.format = format;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        createInfo.usage = usage;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        checkVk(vkCreateImage(context_.device(), &createInfo, nullptr, &image.image), "Failed to create image.");

        VkMemoryRequirements memoryRequirements{};
        vkGetImageMemoryRequirements(context_.device(), image.image, &memoryRequirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex =
            findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        checkVk(vkAllocateMemory(context_.device(), &allocateInfo, nullptr, &image.memory),
                "Failed to allocate image memory.");
        vkBindImageMemory(context_.device(), image.image, image.memory, 0);

        image.view = createImageView(image.image, image.format, image.aspectMask);
        return image;
    }

    void VulkanResourceManager::destroyImage(VulkanImage& image) const {
        if (image.view != VK_NULL_HANDLE) {
            vkDestroyImageView(context_.device(), image.view, nullptr);
            image.view = VK_NULL_HANDLE;
        }

        if (image.image != VK_NULL_HANDLE) {
            vkDestroyImage(context_.device(), image.image, nullptr);
            image.image = VK_NULL_HANDLE;
        }

        if (image.memory != VK_NULL_HANDLE) {
            vkFreeMemory(context_.device(), image.memory, nullptr);
            image.memory = VK_NULL_HANDLE;
        }

        image.format = VK_FORMAT_UNDEFINED;
        image.aspectMask = 0;
    }

    VkImageView VulkanResourceManager::createImageView(VkImage image, VkFormat format,
                                                       VkImageAspectFlags aspectMask) const {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = format;
        createInfo.subresourceRange.aspectMask = aspectMask;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        VkImageView imageView = VK_NULL_HANDLE;
        checkVk(vkCreateImageView(context_.device(), &createInfo, nullptr, &imageView), "Failed to create image view.");
        return imageView;
    }

    VkFormat VulkanResourceManager::findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                                        VkFormatFeatureFlags features) const {
        for (VkFormat format : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &properties);

            if (tiling == VK_IMAGE_TILING_LINEAR && (properties.linearTilingFeatures & features) == features) {
                return format;
            }

            if (tiling == VK_IMAGE_TILING_OPTIMAL && (properties.optimalTilingFeatures & features) == features) {
                return format;
            }
        }

        throw std::runtime_error("Failed to find supported Vulkan format.");
    }

    VkFormat VulkanResourceManager::findDepthFormat() const {
        return findSupportedFormat({VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                                   VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    std::uint32_t VulkanResourceManager::findMemoryType(std::uint32_t typeFilter,
                                                        VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice(), &memoryProperties);

        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const bool typeMatches = (typeFilter & (1U << i)) != 0;
            const bool propertiesMatch = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;

            if (typeMatches && propertiesMatch) {
                return i;
            }
        }

        throw std::runtime_error("Failed to find suitable Vulkan memory type.");
    }

} // namespace lumin::render

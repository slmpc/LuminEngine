#include "lumin/render/VulkanResources.hpp"

#include "lumin/render/VulkanContext.hpp"

#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

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

        try {
            VkMemoryRequirements memoryRequirements{};
            vkGetBufferMemoryRequirements(context_.device(), buffer.buffer, &memoryRequirements);

            VkMemoryAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocateInfo.allocationSize = memoryRequirements.size;
            allocateInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, properties);

            checkVk(vkAllocateMemory(context_.device(), &allocateInfo, nullptr, &buffer.memory),
                    "Failed to allocate buffer memory.");
            checkVk(vkBindBufferMemory(context_.device(), buffer.buffer, buffer.memory, 0),
                    "Failed to bind buffer memory.");
        } catch (...) {
            vkDestroyBuffer(context_.device(), buffer.buffer, nullptr);
            if (buffer.memory != VK_NULL_HANDLE) {
                vkFreeMemory(context_.device(), buffer.memory, nullptr);
            }
            throw;
        }
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
                                                   VkImageUsageFlags usage, VkImageAspectFlags aspectMask,
                                                   std::uint32_t mipLevels, std::uint32_t arrayLayers,
                                                   VkImageViewType viewType) const {
        if (width == 0 || height == 0 || mipLevels == 0 || arrayLayers == 0) {
            throw std::invalid_argument("Vulkan images require non-zero dimensions, mip levels, and array layers.");
        }

        VulkanImage image;
        image.format = format;
        image.aspectMask = aspectMask;
        image.width = width;
        image.height = height;
        image.mipLevels = mipLevels;
        image.arrayLayers = arrayLayers;

        VkImageCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.extent.width = width;
        createInfo.extent.height = height;
        createInfo.extent.depth = 1;
        createInfo.mipLevels = mipLevels;
        createInfo.arrayLayers = arrayLayers;
        createInfo.format = format;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        createInfo.usage = usage;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        checkVk(vkCreateImage(context_.device(), &createInfo, nullptr, &image.image), "Failed to create image.");

        try {
            VkMemoryRequirements memoryRequirements{};
            vkGetImageMemoryRequirements(context_.device(), image.image, &memoryRequirements);

            VkMemoryAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocateInfo.allocationSize = memoryRequirements.size;
            allocateInfo.memoryTypeIndex =
                findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            checkVk(vkAllocateMemory(context_.device(), &allocateInfo, nullptr, &image.memory),
                    "Failed to allocate image memory.");
            checkVk(vkBindImageMemory(context_.device(), image.image, image.memory, 0), "Failed to bind image memory.");
            image.view = createImageView(image.image, image.format, image.aspectMask, mipLevels, arrayLayers, viewType);
        } catch (...) {
            vkDestroyImage(context_.device(), image.image, nullptr);
            if (image.memory != VK_NULL_HANDLE) {
                vkFreeMemory(context_.device(), image.memory, nullptr);
            }
            throw;
        }
        return image;
    }

    void VulkanResourceManager::uploadImage(const VulkanImage& image, const void* pixels,
                                            VkDeviceSize bytesPerLayer) const {
        if (image.image == VK_NULL_HANDLE || image.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
            throw std::invalid_argument("Image upload requires a valid color image.");
        }
        if (pixels == nullptr || bytesPerLayer == 0 || image.arrayLayers == 0 || image.mipLevels != 1) {
            throw std::invalid_argument("Image upload requires a single-mip image and non-empty pixel data.");
        }
        if (bytesPerLayer > std::numeric_limits<VkDeviceSize>::max() / image.arrayLayers) {
            throw std::overflow_error("Image upload size exceeds Vulkan's device-size range.");
        }

        const VkDeviceSize uploadBytes = bytesPerLayer * image.arrayLayers;
        const VkMemoryPropertyFlags stagingMemory =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VulkanBuffer staging = createBuffer(uploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stagingMemory);
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

        try {
            writeBuffer(staging, pixels, uploadBytes);

            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = context_.commandPool();
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            checkVk(vkAllocateCommandBuffers(context_.device(), &allocateInfo, &commandBuffer),
                    "Failed to allocate an image upload command buffer.");

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin an image upload command buffer.");

            VkImageMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = image.image;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.baseMipLevel = 0;
            toTransfer.subresourceRange.levelCount = image.mipLevels;
            toTransfer.subresourceRange.baseArrayLayer = 0;
            toTransfer.subresourceRange.layerCount = image.arrayLayers;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toTransfer);

            std::vector<VkBufferImageCopy> regions(image.arrayLayers);
            for (std::uint32_t layer = 0; layer < image.arrayLayers; ++layer) {
                VkBufferImageCopy& region = regions[layer];
                region.bufferOffset = bytesPerLayer * layer;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = layer;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = VkExtent3D{image.width, image.height, 1};
            }
            vkCmdCopyBufferToImage(commandBuffer, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(regions.size()), regions.data());

            VkImageMemoryBarrier toShaderRead = toTransfer;
            toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

            checkVk(vkEndCommandBuffer(commandBuffer), "Failed to end an image upload command buffer.");
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            checkVk(vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE),
                    "Failed to submit an image upload.");
            checkVk(vkQueueWaitIdle(context_.graphicsQueue()), "Failed to wait for an image upload.");
        } catch (...) {
            if (commandBuffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(context_.device(), context_.commandPool(), 1, &commandBuffer);
            }
            destroyBuffer(staging);
            throw;
        }

        vkFreeCommandBuffers(context_.device(), context_.commandPool(), 1, &commandBuffer);
        destroyBuffer(staging);
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
        image.width = 0;
        image.height = 0;
        image.mipLevels = 1;
        image.arrayLayers = 1;
    }

    VkImageView VulkanResourceManager::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectMask,
                                                       std::uint32_t mipLevels, std::uint32_t arrayLayers,
                                                       VkImageViewType viewType) const {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = viewType;
        createInfo.format = format;
        createInfo.subresourceRange.aspectMask = aspectMask;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = mipLevels;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = arrayLayers;

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

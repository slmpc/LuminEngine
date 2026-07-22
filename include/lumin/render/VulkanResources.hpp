#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace lumin::render {

    class VulkanContext;

    struct VulkanBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    struct VulkanImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspectMask = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mipLevels = 1;
        std::uint32_t arrayLayers = 1;
    };

    class VulkanResourceManager {
    public:
        explicit VulkanResourceManager(VulkanContext& context);

        [[nodiscard]] VulkanBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                VkMemoryPropertyFlags properties) const;
        void destroyBuffer(VulkanBuffer& buffer) const;
        void writeBuffer(const VulkanBuffer& buffer, const void* data, VkDeviceSize size) const;

        [[nodiscard]] VulkanImage createImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                                              VkImageUsageFlags usage, VkImageAspectFlags aspectMask,
                                              std::uint32_t mipLevels = 1, std::uint32_t arrayLayers = 1,
                                              VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D) const;
        void uploadImage(const VulkanImage& image, const void* pixels, VkDeviceSize bytesPerLayer) const;
        void destroyImage(VulkanImage& image) const;
        [[nodiscard]] VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectMask,
                                                  std::uint32_t mipLevels = 1, std::uint32_t arrayLayers = 1,
                                                  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D) const;

        [[nodiscard]] VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                                   VkFormatFeatureFlags features) const;
        [[nodiscard]] VkFormat findDepthFormat() const;
        [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    private:
        VulkanContext& context_;
    };

} // namespace lumin::render

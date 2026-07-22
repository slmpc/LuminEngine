#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "lumin/render/VulkanResources.hpp"

namespace lumin::render {

    class VulkanContext;

    struct TextureFrameResources {
        VulkanImage position;
        VulkanImage normalRoughness;
        VulkanImage albedo;
        VulkanImage depth;
    };

    class TextureManager {
    public:
        static constexpr std::uint32_t maxFramesInFlight = 2;

        explicit TextureManager(VulkanContext& context);
        ~TextureManager();

        TextureManager(const TextureManager&) = delete;
        TextureManager& operator=(const TextureManager&) = delete;

        void create(VkExtent2D extent);
        void destroy() noexcept;

        [[nodiscard]] const TextureFrameResources& frame(std::uint32_t frameIndex) const;
        [[nodiscard]] VkFormat positionFormat() const noexcept;
        [[nodiscard]] VkFormat normalFormat() const noexcept;
        [[nodiscard]] VkFormat albedoFormat() const noexcept;
        [[nodiscard]] VkFormat depthFormat() const noexcept;
        [[nodiscard]] VkSampler sampler() const noexcept;
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept;
        [[nodiscard]] VkDescriptorSet descriptorSet(std::uint32_t frameIndex) const;

    private:
        [[nodiscard]] VkFormat chooseFormat(const std::vector<VkFormat>& candidates) const;
        void createImages(VkExtent2D extent);
        void createSamplerAndDescriptors();

        VulkanContext& context_;
        VulkanResourceManager resources_;
        std::array<TextureFrameResources, maxFramesInFlight> frames_{};
        VkFormat positionFormat_ = VK_FORMAT_UNDEFINED;
        VkFormat normalFormat_ = VK_FORMAT_UNDEFINED;
        VkFormat albedoFormat_ = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, maxFramesInFlight> descriptorSets_{};
    };

}

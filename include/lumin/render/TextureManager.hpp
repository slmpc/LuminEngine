#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <vulkan/vulkan.h>

#include "lumin/render/VulkanResources.hpp"

namespace lumin::render {

    class VulkanContext;

    inline constexpr std::uint32_t shadowCascadeCount = 4;
    inline constexpr std::uint32_t shadowMapResolution = 2048;

    struct alignas(16) PostProcessUniforms {
        glm::mat4 inverseViewProjection{1.0f};
        glm::mat4 viewProjection{1.0f};
        std::array<glm::mat4, shadowCascadeCount> cascadeViewProjections{};
        glm::vec4 cascadeSplits{0.0f};
        glm::vec4 cameraPosition{0.0f};
        glm::vec4 cameraForward{0.0f, 0.0f, -1.0f, 0.0f};
        glm::vec4 lightDirection{-0.45f, -0.8f, -0.35f, 0.0f};
        glm::vec4 renderSize{1.0f};
        glm::vec4 renderOptions{0.0f};
        glm::vec4 tonemapOptions{1.0f, 0.0f, 0.0f, 0.0f};
    };

    static_assert(sizeof(PostProcessUniforms) % 16 == 0);
    static_assert(alignof(PostProcessUniforms) == 16);

    struct TextureFrameResources {
        VulkanImage position;
        VulkanImage normalRoughness;
        VulkanImage albedo;
        VulkanImage motion;
        VulkanImage depth;
        VulkanImage ambientOcclusion;
        VulkanImage lighting;
        VulkanImage taaResolved;
        VulkanImage history;
        std::array<VulkanImage, shadowCascadeCount> shadowCascades{};
        VulkanBuffer postProcessUniform;
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
        void updatePostProcessUniforms(std::uint32_t frameIndex, const PostProcessUniforms& uniforms);
        void invalidateHistory() noexcept;
        void markHistoryValid(std::uint32_t frameIndex);

        [[nodiscard]] const TextureFrameResources& frame(std::uint32_t frameIndex) const;
        [[nodiscard]] bool historyValid(std::uint32_t frameIndex) const;
        [[nodiscard]] bool historyInitialized(std::uint32_t frameIndex) const;
        [[nodiscard]] VkFormat positionFormat() const noexcept;
        [[nodiscard]] VkFormat normalFormat() const noexcept;
        [[nodiscard]] VkFormat albedoFormat() const noexcept;
        [[nodiscard]] VkFormat motionFormat() const noexcept;
        [[nodiscard]] VkFormat depthFormat() const noexcept;
        [[nodiscard]] VkFormat ambientOcclusionFormat() const noexcept;
        [[nodiscard]] VkFormat lightingFormat() const noexcept;
        [[nodiscard]] VkFormat shadowDepthFormat() const noexcept;
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
        VkFormat motionFormat_ = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
        VkFormat ambientOcclusionFormat_ = VK_FORMAT_UNDEFINED;
        VkFormat lightingFormat_ = VK_FORMAT_UNDEFINED;
        VkFormat shadowDepthFormat_ = VK_FORMAT_UNDEFINED;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, maxFramesInFlight> descriptorSets_{};
        std::array<bool, maxFramesInFlight> historyValid_{};
        std::array<bool, maxFramesInFlight> historyInitialized_{};
    };

} // namespace lumin::render

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "lumin/render/VulkanResources.hpp"

namespace lumin::render {

    class VulkanContext;

    inline constexpr std::uint32_t shadowCascadeCount = 4;
    inline constexpr std::uint32_t shadowMapResolution = 2048;
    inline constexpr std::uint32_t fullscreenSampledImageCount = 8 + shadowCascadeCount;
    inline constexpr std::uint32_t fullscreenSamplerBinding = fullscreenSampledImageCount;
    inline constexpr std::uint32_t fullscreenUniformBinding = fullscreenSampledImageCount + 1;

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
        GpuTexture position;
        GpuTexture normalRoughness;
        GpuTexture albedo;
        GpuTexture motion;
        GpuTexture depth;
        GpuTexture globalIllumination;
        GpuTexture lighting;
        GpuTexture taaResolved;
        GpuTexture history;
        std::array<GpuTexture, shadowCascadeCount> shadowCascades{};
        GpuBuffer postProcessUniform;
    };

    class TextureManager {
    public:
        static constexpr std::uint32_t maxFramesInFlight = 2;

        explicit TextureManager(VulkanContext& context);
        explicit TextureManager(nvrhi::IDevice& device);
        ~TextureManager();

        TextureManager(const TextureManager&) = delete;
        TextureManager& operator=(const TextureManager&) = delete;

        void create(std::uint32_t width, std::uint32_t height);
        void destroy() noexcept;
        void updatePostProcessUniforms(std::uint32_t frameIndex, const PostProcessUniforms& uniforms);
        void invalidateHistory() noexcept;
        void markHistoryValid(std::uint32_t frameIndex);

        [[nodiscard]] const TextureFrameResources& frame(std::uint32_t frameIndex) const;
        [[nodiscard]] bool historyValid(std::uint32_t frameIndex) const;
        [[nodiscard]] bool historyInitialized(std::uint32_t frameIndex) const;
        [[nodiscard]] nvrhi::ResourceStates historyInitialState(std::uint32_t frameIndex) const;
        [[nodiscard]] nvrhi::Format positionFormat() const noexcept;
        [[nodiscard]] nvrhi::Format normalFormat() const noexcept;
        [[nodiscard]] nvrhi::Format albedoFormat() const noexcept;
        [[nodiscard]] nvrhi::Format motionFormat() const noexcept;
        [[nodiscard]] nvrhi::Format depthFormat() const noexcept;
        [[nodiscard]] nvrhi::Format globalIlluminationFormat() const noexcept;
        [[nodiscard]] nvrhi::Format lightingFormat() const noexcept;
        [[nodiscard]] nvrhi::Format shadowDepthFormat() const noexcept;
        [[nodiscard]] nvrhi::SamplerHandle sampler() const noexcept;
        [[nodiscard]] nvrhi::BindingLayoutHandle bindingLayout() const noexcept;
        [[nodiscard]] nvrhi::BindingSetHandle bindingSet(std::uint32_t frameIndex) const;

    private:
        [[nodiscard]] nvrhi::Format chooseFormat(std::span<const nvrhi::Format> candidates,
                                                 nvrhi::FormatSupport required) const;
        [[nodiscard]] GpuTexture createTexture(const nvrhi::TextureDesc& desc) const;
        void createImages(std::uint32_t width, std::uint32_t height);
        void createSamplerAndBindings();

        nvrhi::IDevice& device_;
        GpuResourceManager resources_;
        std::array<TextureFrameResources, maxFramesInFlight> frames_{};
        nvrhi::Format positionFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format normalFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format albedoFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format motionFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format depthFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format globalIlluminationFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format lightingFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format shadowDepthFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::SamplerHandle sampler_;
        nvrhi::BindingLayoutHandle bindingLayout_;
        std::array<nvrhi::BindingSetHandle, maxFramesInFlight> bindingSets_{};
        std::array<bool, maxFramesInFlight> historyValid_{};
        std::array<bool, maxFramesInFlight> historyInitialized_{};
    };

} // namespace lumin::render

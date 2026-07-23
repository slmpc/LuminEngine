#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <vulkan/vulkan.h>

#include "lumin/render/FrameGraph.hpp"

namespace lumin::scene {
    class Level;
}

namespace lumin::render {
    class VulkanContext;
}

namespace lumin::render::gi {

    inline constexpr std::uint32_t indirectRadianceFirstChannel = 0;
    inline constexpr std::uint32_t ambientVisibilityChannel = 3;
    inline constexpr std::array<float, 4> neutralOutput = {0.0f, 0.0f, 0.0f, 1.0f};

    struct BackendInfo {
        std::string_view name;
        bool temporal = false;
        bool hardwareRayTracing = false;
    };

    struct FrameResources {
        VkImageView positionView = VK_NULL_HANDLE;
        VkImageView normalRoughnessView = VK_NULL_HANDLE;
        VkImageView albedoMetallicView = VK_NULL_HANDLE;
        VkImageView motionView = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkBuffer uniformBuffer = VK_NULL_HANDLE;
        VkImage outputImage = VK_NULL_HANDLE;
        VkImageView outputView = VK_NULL_HANDLE;
    };

    struct CreateInfo {
        VulkanContext& context;
        VkExtent2D extent{};
        VkFormat outputFormat = VK_FORMAT_UNDEFINED;
        VkSampler sampler = VK_NULL_HANDLE;
        std::span<const FrameResources> frames;
    };

    struct FrameInfo {
        const scene::Level& level;
        std::uint32_t frameIndex = 0;
        std::uint64_t frameNumber = 0;
        bool enabled = true;
        bool cameraCut = false;
        VkExtent2D extent{};
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        FrameGraphResourceHandle motion;
        FrameGraphResourceHandle depth;
        FrameGraphResourceHandle output;
    };

    struct HistoryInvalidationState {
        bool cameraCut = false;
        bool topologyChanged = false;
        bool backendReenabled = false;
        bool swapchainRecreated = false;
    };

    [[nodiscard]] constexpr bool shouldInvalidateHistory(const HistoryInvalidationState& state) noexcept {
        return state.cameraCut || state.topologyChanged || state.backendReenabled || state.swapchainRecreated;
    }

    class GlobalIlluminationBackend {
    public:
        GlobalIlluminationBackend() = default;
        virtual ~GlobalIlluminationBackend() = default;

        GlobalIlluminationBackend(const GlobalIlluminationBackend&) = delete;
        GlobalIlluminationBackend& operator=(const GlobalIlluminationBackend&) = delete;
        GlobalIlluminationBackend(GlobalIlluminationBackend&&) = delete;
        GlobalIlluminationBackend& operator=(GlobalIlluminationBackend&&) = delete;

        [[nodiscard]] virtual BackendInfo info() const noexcept = 0;
        virtual void create(const CreateInfo& createInfo) = 0;
        virtual void destroy() noexcept = 0;
        virtual void invalidateHistory() noexcept = 0;
        virtual void addPasses(FrameGraph& frameGraph, const FrameInfo& frameInfo) = 0;
    };

} // namespace lumin::render::gi

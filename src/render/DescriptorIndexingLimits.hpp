#pragma once

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

namespace lumin::render::detail {

    inline constexpr std::uint32_t maxMaterialTextureDescriptorCount = 1U << 24U;

    struct DescriptorIndexingPlan {
        std::uint32_t materialTextureCount = 0;
        std::uint32_t gbufferSetCount = 0;
        std::uint32_t shadowSetCount = 0;
        std::uint32_t totalSetCount = 0;
        std::uint32_t sampledImageDescriptorCount = 0;
        std::uint32_t samplerDescriptorCount = 0;
    };

    [[nodiscard]] DescriptorIndexingPlan makeDescriptorIndexingPlan(const VkPhysicalDeviceLimits& limits,
                                                                    std::size_t uniqueMaterialCount,
                                                                    std::uint32_t frameCount,
                                                                    std::uint32_t shadowCascadeCount);

    void validateMaterialImageDimensions(const VkPhysicalDeviceLimits& limits, std::uint32_t width,
                                         std::uint32_t height);

} // namespace lumin::render::detail

#include "DescriptorIndexingLimits.hpp"

#include <limits>
#include <stdexcept>

namespace lumin::render::detail {
    namespace {

        std::uint32_t checkedU32(std::uint64_t value, const char* message) {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(message);
            }
            return static_cast<std::uint32_t>(value);
        }

        std::uint32_t checkedMultiply(std::uint32_t left, std::uint32_t right, const char* message) {
            return checkedU32(static_cast<std::uint64_t>(left) * right, message);
        }

        std::uint32_t checkedAdd(std::uint32_t left, std::uint32_t right, const char* message) {
            return checkedU32(static_cast<std::uint64_t>(left) + right, message);
        }

    } // namespace

    DescriptorIndexingPlan makeDescriptorIndexingPlan(const VkPhysicalDeviceLimits& limits,
                                                      std::size_t uniqueMaterialCount, std::uint32_t frameCount,
                                                      std::uint32_t shadowCascadeCount) {
        if (frameCount == 0) {
            throw std::invalid_argument("Descriptor indexing requires at least one frame slot.");
        }
        if (uniqueMaterialCount >= maxMaterialTextureDescriptorCount) {
            throw std::length_error("Material texture descriptor indices exceed exact float representation.");
        }

        const std::uint32_t materialTextureCount = static_cast<std::uint32_t>(uniqueMaterialCount + 1);
        const std::uint64_t sampledImagesPerSet = static_cast<std::uint64_t>(materialTextureCount) * 2;
        if (sampledImagesPerSet > limits.maxPerStageDescriptorSampledImages) {
            throw std::length_error("Material textures exceed maxPerStageDescriptorSampledImages.");
        }
        if (sampledImagesPerSet > limits.maxDescriptorSetSampledImages) {
            throw std::length_error("Material textures exceed maxDescriptorSetSampledImages.");
        }
        if (limits.maxPerStageResources == 0 || sampledImagesPerSet > limits.maxPerStageResources - 1ULL) {
            throw std::length_error("Material textures exceed maxPerStageResources.");
        }
        if (limits.maxPerStageDescriptorSamplers == 0 || limits.maxDescriptorSetSamplers == 0) {
            throw std::length_error("Material textures require one fragment-stage sampler descriptor.");
        }

        const std::uint32_t shadowSetCount =
            checkedMultiply(frameCount, shadowCascadeCount, "Shadow descriptor set count exceeds uint32 range.");
        const std::uint32_t totalSetCount =
            checkedAdd(frameCount, shadowSetCount, "Model descriptor set count exceeds uint32 range.");
        const std::uint32_t sampledImageDescriptorCount =
            checkedU32(static_cast<std::uint64_t>(frameCount) * sampledImagesPerSet,
                       "Material descriptor pool sampled-image count exceeds uint32 range.");
        return DescriptorIndexingPlan{
            .materialTextureCount = materialTextureCount,
            .gbufferSetCount = frameCount,
            .shadowSetCount = shadowSetCount,
            .totalSetCount = totalSetCount,
            .sampledImageDescriptorCount = sampledImageDescriptorCount,
            .samplerDescriptorCount = frameCount,
        };
    }

    void validateMaterialImageDimensions(const VkPhysicalDeviceLimits& limits, std::uint32_t width,
                                         std::uint32_t height) {
        if (width == 0 || height == 0) {
            throw std::invalid_argument("Material textures require non-zero dimensions.");
        }
        if (width > limits.maxImageDimension2D || height > limits.maxImageDimension2D) {
            throw std::length_error("Material texture dimensions exceed maxImageDimension2D.");
        }
    }

} // namespace lumin::render::detail

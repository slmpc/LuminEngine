#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "lumin/render/ModelRenderer.hpp"

namespace lumin::render::detail {

    inline constexpr std::uint32_t maxMaterialTextureDescriptorCount = 1U << 24U;
    inline constexpr std::uint32_t maxMaterialTextureBindingArraySize = std::numeric_limits<std::uint16_t>::max();

    struct DescriptorIndexingLimits {
        std::uint32_t maxMaterialTextures = maxMaterialTextureBindingArraySize;
        std::uint32_t maxDrawIndirectCount = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t maxImageDimension2D = std::numeric_limits<std::uint32_t>::max();
    };

    struct DescriptorIndexingPlan {
        std::uint32_t materialTextureCount = 0;
        std::uint32_t gbufferSetCount = 0;
        std::uint32_t shadowSetCount = 0;
        std::uint32_t totalSetCount = 0;
        std::uint32_t sampledImageDescriptorCount = 0;
        std::uint32_t samplerDescriptorCount = 0;
    };

    struct ModelRendererMaterialImageDimensions {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    enum class ModelRendererBindingKind {
        ConstantBuffer,
        StructuredBuffer,
        Texture,
        Sampler,
    };

    struct ModelRendererBindingItem {
        ModelRendererBindingKind kind;
        std::uint32_t binding;
        std::uint32_t arrayLength = 1;
    };

    struct ModelRendererBindingContract {
        std::array<ModelRendererBindingItem, 5> gbufferItems;
        std::array<ModelRendererBindingItem, 2> shadowItems;
        std::uint32_t gbufferSetCount = 0;
        std::uint32_t shadowSetCount = 0;
    };

    class FrameSlotReadiness {
    public:
        explicit FrameSlotReadiness(std::uint32_t frameCount = 0);

        void markReady(std::uint32_t frameIndex);
        void requireReady(std::uint32_t frameIndex) const;
        void consumeReady(std::uint32_t frameIndex);

    private:
        std::vector<bool> ready_;
    };

    [[nodiscard]] DescriptorIndexingLimits toDescriptorIndexingLimits(const ModelRendererCapabilities& capabilities);
    [[nodiscard]] ModelRendererBindingContract makeModelRendererBindingContract(const DescriptorIndexingPlan& plan);
    [[nodiscard]] bool requiresPreviousModelReset(bool resetMotion, bool hasPreviousModels) noexcept;

    [[nodiscard]] DescriptorIndexingPlan
    makeDescriptorIndexingPlan(const DescriptorIndexingLimits& limits, std::size_t uniqueMaterialCount,
                               std::uint32_t frameCount, std::uint32_t shadowCascadeCount, std::size_t drawCount);

    void validateMaterialImageDimensions(const DescriptorIndexingLimits& limits, std::uint32_t width,
                                         std::uint32_t height);

    template <typename Callback>
    DescriptorIndexingPlan createModelRendererResourcesAfterPreflight(
        const ModelRendererCapabilities& capabilities, std::size_t uniqueMaterialCount, std::uint32_t frameCount,
        std::uint32_t shadowCascadeCount, std::size_t drawCount,
        std::span<const ModelRendererMaterialImageDimensions> materialImages, Callback&& callback) {
        const DescriptorIndexingLimits limits = toDescriptorIndexingLimits(capabilities);
        const DescriptorIndexingPlan plan = makeDescriptorIndexingPlan(
            limits, uniqueMaterialCount, frameCount, shadowCascadeCount, drawCount);
        validateMaterialImageDimensions(limits, 1, 1);
        for (const ModelRendererMaterialImageDimensions image : materialImages) {
            validateMaterialImageDimensions(limits, image.width, image.height);
        }
        std::forward<Callback>(callback)(plan);
        return plan;
    }

    template <typename Callback>
    DescriptorIndexingPlan createModelRendererResourcesAfterPreflight(
        const ModelRendererCapabilities& capabilities, std::size_t uniqueMaterialCount, std::uint32_t frameCount,
        std::uint32_t shadowCascadeCount, std::size_t drawCount, Callback&& callback) {
        return createModelRendererResourcesAfterPreflight(
            capabilities, uniqueMaterialCount, frameCount, shadowCascadeCount, drawCount,
            std::span<const ModelRendererMaterialImageDimensions>{}, std::forward<Callback>(callback));
    }

    template <typename Callback>
    void forEachMaterialTextureArrayElement(const ModelRendererBindingContract& contract, Callback&& callback) {
        const ModelRendererBindingItem& baseColor = contract.gbufferItems[2];
        const ModelRendererBindingItem& normalRoughness = contract.gbufferItems[3];
        for (std::uint32_t arrayElement = 0; arrayElement < baseColor.arrayLength; ++arrayElement) {
            callback(baseColor.binding, normalRoughness.binding, arrayElement);
        }
    }

} // namespace lumin::render::detail

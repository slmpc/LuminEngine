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

    DescriptorIndexingLimits toDescriptorIndexingLimits(const ModelRendererCapabilities& capabilities) {
        return DescriptorIndexingLimits{
            .maxMaterialTextures = capabilities.maxMaterialTextureArrayLength,
            .maxDrawIndirectCount = capabilities.maxDrawIndirectCount,
            .maxImageDimension2D = capabilities.maxImageDimension2D,
        };
    }

    ModelRendererBindingContract makeModelRendererBindingContract(const DescriptorIndexingPlan& plan) {
        if (plan.materialTextureCount == 0) {
            throw std::invalid_argument("Model renderer bindings require a fallback material texture.");
        }
        return ModelRendererBindingContract{
            .gbufferItems = {
                ModelRendererBindingItem{ModelRendererBindingKind::ConstantBuffer, 0},
                ModelRendererBindingItem{ModelRendererBindingKind::StructuredBuffer, 1},
                ModelRendererBindingItem{ModelRendererBindingKind::Texture, 2, plan.materialTextureCount},
                ModelRendererBindingItem{ModelRendererBindingKind::Texture, 3, plan.materialTextureCount},
                ModelRendererBindingItem{ModelRendererBindingKind::Sampler, 4},
            },
            .shadowItems = {
                ModelRendererBindingItem{ModelRendererBindingKind::ConstantBuffer, 0},
                ModelRendererBindingItem{ModelRendererBindingKind::StructuredBuffer, 1},
            },
            .gbufferSetCount = plan.gbufferSetCount,
            .shadowSetCount = plan.shadowSetCount,
        };
    }

    FrameSlotReadiness::FrameSlotReadiness(std::uint32_t frameCount) : ready_(frameCount, false) {
        if (frameCount == 0) {
            throw std::invalid_argument("ModelRenderer requires at least one frame slot.");
        }
    }

    void FrameSlotReadiness::markReady(std::uint32_t frameIndex) {
        if (frameIndex >= ready_.size()) {
            throw std::out_of_range("ModelRenderer frame index is out of range.");
        }
        ready_[frameIndex] = true;
    }

    void FrameSlotReadiness::requireReady(std::uint32_t frameIndex) const {
        if (frameIndex >= ready_.size()) {
            throw std::out_of_range("ModelRenderer frame index is out of range.");
        }
        if (!ready_[frameIndex]) {
            throw std::logic_error("ModelRenderer frame slot must be synchronized after caller readiness.");
        }
    }

    void FrameSlotReadiness::consumeReady(std::uint32_t frameIndex) {
        requireReady(frameIndex);
        ready_[frameIndex] = false;
    }

    bool requiresPreviousModelReset(bool resetMotion, bool hasPreviousModels) noexcept {
        return resetMotion || !hasPreviousModels;
    }

    DescriptorIndexingPlan makeDescriptorIndexingPlan(const DescriptorIndexingLimits& limits,
                                                      std::size_t uniqueMaterialCount, std::uint32_t frameCount,
                                                      std::uint32_t shadowCascadeCount, std::size_t drawCount) {
        if (frameCount == 0) {
            throw std::invalid_argument("Descriptor indexing requires at least one frame slot.");
        }
        if (shadowCascadeCount == 0) {
            throw std::invalid_argument("Descriptor indexing requires at least one shadow cascade.");
        }
        if (uniqueMaterialCount >= maxMaterialTextureDescriptorCount) {
            throw std::length_error("Material texture descriptor indices exceed exact float representation.");
        }
        if (uniqueMaterialCount >= limits.maxMaterialTextures ||
            uniqueMaterialCount >= maxMaterialTextureBindingArraySize) {
            throw std::length_error("Material textures exceed the NvRHI binding-array size.");
        }
        if (drawCount > limits.maxDrawIndirectCount) {
            throw std::length_error("Level model count exceeds maxDrawIndirectCount.");
        }

        const std::uint32_t materialTextureCount = static_cast<std::uint32_t>(uniqueMaterialCount + 1);
        const std::uint64_t sampledImagesPerSet = static_cast<std::uint64_t>(materialTextureCount) * 2;
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

    void validateMaterialImageDimensions(const DescriptorIndexingLimits& limits, std::uint32_t width,
                                         std::uint32_t height) {
        if (width == 0 || height == 0) {
            throw std::invalid_argument("Material textures require non-zero dimensions.");
        }
        if (width > limits.maxImageDimension2D || height > limits.maxImageDimension2D) {
            throw std::length_error("Material texture dimensions exceed maxImageDimension2D.");
        }
    }

} // namespace lumin::render::detail

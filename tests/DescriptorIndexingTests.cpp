#include "lumin/render/ModelRenderer.hpp"
#include "lumin/scene/Level.hpp"
#include "render/DescriptorIndexingLimits.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function>
    void requireThrows(Function&& function, const std::string& message) {
        bool rejected = false;
        try {
            function();
        } catch (const Exception&) {
            rejected = true;
        }
        require(rejected, message);
    }

    lumin::assets::Mesh makeTriangle() {
        lumin::assets::Mesh mesh;
        mesh.vertices = {
            {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
            {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
            {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 1.0f}},
        };
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    lumin::scene::Material texturedMaterial(const std::string& stem, bool flipNormalY = true) {
        lumin::scene::Material material;
        material.textures = lumin::scene::PbrTextureSet{
            .baseColor = stem + "-base.png",
            .normal = stem + "-normal.png",
            .roughness = stem + "-roughness.png",
            .flipNormalY = flipNormalY,
        };
        return material;
    }

    void testMaterialDescriptorIndicesPreserveBatchAbi() {
        static_assert(sizeof(lumin::render::ObjectData) == 224);
        static_assert(alignof(lumin::render::ObjectData) == 16);

        lumin::scene::Level level;
        const lumin::scene::MeshHandle mesh = level.addMesh(makeTriangle());
        level.addModel(mesh, {}, texturedMaterial("stone"));
        level.addModel(mesh, {}, texturedMaterial("stone", false));
        level.addModel(mesh, {}, texturedMaterial("wood"));
        level.addModel(mesh);

        const lumin::render::ModelBatch batch = lumin::render::ModelRenderer::buildBatch(level);
        require(batch.objects.size() == 4, "Every model must retain one ObjectData record.");
        require(batch.objects[0].materialParameters.z == 1.0f && batch.objects[1].materialParameters.z == 1.0f,
                "Materials referencing the same images must share descriptor index 1.");
        require(batch.objects[2].materialParameters.z == 2.0f,
                "The second unique material texture set must use descriptor index 2.");
        require(batch.objects[3].materialParameters.z == 0.0f,
                "Untextured materials must retain descriptor index 0 fallback semantics.");
    }

    void testDescriptorLimitsRejectOversubscription() {
        VkPhysicalDeviceLimits limits{};
        limits.maxPerStageDescriptorSamplers = 1;
        limits.maxPerStageDescriptorSampledImages = 64;
        limits.maxDescriptorSetSamplers = 1;
        limits.maxDescriptorSetSampledImages = 64;
        limits.maxPerStageResources = 65;
        limits.maxImageDimension2D = 4096;

        const lumin::render::detail::DescriptorIndexingPlan plan =
            lumin::render::detail::makeDescriptorIndexingPlan(limits, 2, 2, 4);
        require(plan.materialTextureCount == 3 && plan.gbufferSetCount == 2 && plan.shadowSetCount == 8 &&
                    plan.totalSetCount == 10 && plan.sampledImageDescriptorCount == 12 &&
                    plan.samplerDescriptorCount == 2,
                "Descriptor planning must account for fallback, frame slots, and split shadow sets.");

        limits.maxPerStageDescriptorSampledImages = 3;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 2, 4);
            },
            "Per-stage sampled-image descriptor oversubscription must be rejected before Vulkan allocation.");

        limits.maxPerStageDescriptorSampledImages = 64;
        limits.maxDescriptorSetSampledImages = 3;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 2, 4);
            },
            "Per-set sampled-image descriptor oversubscription must be rejected before Vulkan allocation.");

        limits.maxDescriptorSetSampledImages = 64;
        limits.maxPerStageResources = 4;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 2, 4);
            },
            "Fragment-stage resource oversubscription must be rejected before Vulkan allocation.");

        limits.maxPerStageResources = 65;
        limits.maxDescriptorSetSamplers = 0;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 2, 4);
            },
            "Missing sampler descriptor capacity must be rejected before Vulkan allocation.");

        limits.maxDescriptorSetSamplers = 1;
        limits.maxPerStageDescriptorSamplers = 0;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 2, 4);
            },
            "Missing per-stage sampler capacity must be rejected before Vulkan allocation.");

        limits.maxPerStageDescriptorSamplers = 1;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(
                    limits, lumin::render::detail::maxMaterialTextureDescriptorCount, 2, 4);
            },
            "Material descriptor indices that exceed exact float representation must be rejected.");

        requireThrows<std::overflow_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1,
                                                                        std::numeric_limits<std::uint32_t>::max(), 4);
            },
            "Descriptor pool count overflow must be rejected before Vulkan allocation.");

        VkPhysicalDeviceLimits largeLimits = limits;
        largeLimits.maxPerStageDescriptorSampledImages = std::numeric_limits<std::uint32_t>::max();
        largeLimits.maxDescriptorSetSampledImages = std::numeric_limits<std::uint32_t>::max();
        largeLimits.maxPerStageResources = std::numeric_limits<std::uint32_t>::max();
        requireThrows<std::overflow_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(largeLimits, 32767, 65536, 0);
            },
            "Sampled-image descriptor pool overflow must be rejected before Vulkan allocation.");

        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 0, 4);
            },
            "Zero frame slots must be rejected before descriptor planning.");

        requireThrows<std::length_error>(
            [&] {
                lumin::render::detail::validateMaterialImageDimensions(limits, 4097, 1);
            },
            "Material images larger than maxImageDimension2D must be rejected.");
        requireThrows<std::invalid_argument>(
            [&] {
                lumin::render::detail::validateMaterialImageDimensions(limits, 0, 1);
            },
            "Zero-width material images must be rejected before Vulkan allocation.");
    }

} // namespace

int main() {
    try {
        testMaterialDescriptorIndicesPreserveBatchAbi();
        testDescriptorLimitsRejectOversubscription();
        std::cout << "Descriptor indexing tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Descriptor indexing test failed: " << exception.what() << '\n';
        return 1;
    }
}

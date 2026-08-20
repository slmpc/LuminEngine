#include "render/ModelRenderer.hpp"
#include "render/world/RenderWorld.hpp"
#include "scene/Level.hpp"
#include "render/resources/DescriptorIndexingLimits.hpp"

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
        static_assert(sizeof(lumin::render::ObjectData) == 240);
        static_assert(alignof(lumin::render::ObjectData) == 16);

        lumin::scene::Level level;
        const lumin::scene::MeshHandle mesh = level.addMesh(makeTriangle());
        level.addModel(mesh, {}, texturedMaterial("stone"));
        level.addModel(mesh, {}, texturedMaterial("stone", false));
        level.addModel(mesh, {}, texturedMaterial("wood"));
        level.addModel(mesh);

        const auto renderWorld = lumin::render::world::RenderWorldExtractor::extract(level);
        const lumin::render::ModelBatch batch = lumin::render::ModelRenderer::buildBatch(*renderWorld);
        require(batch.objects.size() == 4, "Every model must retain one ObjectData record.");
        require(batch.objects[0].materialParameters.z == 1.0f && batch.objects[1].materialParameters.z == 1.0f,
                "Materials referencing the same images must share descriptor index 1.");
        require(batch.objects[2].materialParameters.z == 2.0f,
                "The second unique material texture set must use descriptor index 2.");
        require(batch.objects[3].materialParameters.z == 0.0f,
                "Untextured materials must retain descriptor index 0 fallback semantics.");
    }

    void testSparseMaterialTableUsesStableModelSlots() {
        lumin::scene::Level level;
        const lumin::scene::MeshHandle mesh = level.addMesh(makeTriangle());
        const lumin::scene::ModelHandle removed = level.addModel(mesh);
        const lumin::scene::ModelHandle retained = level.addModel(mesh);
        require(level.removeModel(removed), "Sparse material fixture must remove its first model slot.");

        const auto world = lumin::render::world::RenderWorldExtractor::extract(level);
        const lumin::render::ModelBatch batch = lumin::render::ModelRenderer::buildBatch(*world);
        require(batch.objects.size() == 1 && batch.materials.size() == static_cast<std::size_t>(retained.index) + 1U &&
                    batch.objects.front().metadata.x == retained.index,
                "Material table and G-buffer object metadata must retain sparse stable model slots.");
    }

    void testDescriptorLimitsRejectOversubscription() {
        lumin::render::detail::DescriptorIndexingLimits limits{};
        limits.maxMaterialTextures = 32;
        limits.maxDrawIndirectCount = 16;
        limits.maxImageDimension2D = 4096;

        const lumin::render::detail::DescriptorIndexingPlan plan =
            lumin::render::detail::makeDescriptorIndexingPlan(limits, 2, 2, 4, 3);
        require(plan.materialTextureCount == 3 && plan.gbufferSetCount == 2 && plan.shadowSetCount == 8 &&
                    plan.totalSetCount == 10 && plan.sampledImageDescriptorCount == 12 &&
                    plan.samplerDescriptorCount == 2,
                "Descriptor planning must account for fallback, frame slots, and split shadow sets.");

        limits.maxMaterialTextures = 2;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 2, 2, 4, 3);
            },
            "NvRHI binding-array oversubscription must be rejected before binding layout creation.");

        limits.maxMaterialTextures = 32;
        limits.maxDrawIndirectCount = 2;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 2, 4, 3);
            },
            "Indirect draw oversubscription must be rejected before buffer or pipeline creation.");

        limits.maxDrawIndirectCount = 16;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(
                    limits, lumin::render::detail::maxMaterialTextureBindingArraySize, 2, 4, 1);
            },
            "The NvRHI uint16 binding-array representation must be enforced.");

        limits.maxMaterialTextures = std::numeric_limits<std::uint32_t>::max();
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(
                    limits, lumin::render::detail::maxMaterialTextureDescriptorCount, 2, 4, 1);
            },
            "Material descriptor indices that exceed exact float representation must be rejected.");

        requireThrows<std::overflow_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(
                    limits, 1, std::numeric_limits<std::uint32_t>::max(), 4, 1);
            },
            "Binding set count overflow must be rejected before allocation.");

        lumin::render::detail::DescriptorIndexingLimits largeLimits = limits;
        requireThrows<std::overflow_error>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(largeLimits, 32767, 65536, 1, 1);
            },
            "Sampled-image binding count overflow must be rejected before allocation.");

        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 0, 4, 1);
            },
            "Zero frame slots must be rejected before descriptor planning.");

        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::detail::makeDescriptorIndexingPlan(limits, 1, 1, 0, 1);
            },
            "Zero shadow cascades must be rejected before descriptor planning.");

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
        testSparseMaterialTableUsesStableModelSlots();
        testDescriptorLimitsRejectOversubscription();
        std::cout << "Descriptor indexing tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Descriptor indexing test failed: " << exception.what() << '\n';
        return 1;
    }
}

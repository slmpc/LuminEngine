#include "render/core/FrameDataContracts.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using namespace lumin::render;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function> void requireThrows(Function&& function, const char* message) {
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    class StubFeature final : public core::IRenderFeature {
    public:
        explicit StubFeature(core::FeatureDescriptor descriptor) : descriptor_(std::move(descriptor)) {
        }

        [[nodiscard]] const core::FeatureDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        void addPasses(core::RenderFeatureFrameContext&) override {
        }

    private:
        core::FeatureDescriptor descriptor_;
    };

    [[nodiscard]] core::RenderFeatureRegistry
    registryFor(const pipelines::DefaultRenderPipelineDefinition& definition) {
        core::RenderFeatureRegistry registry;
        for (const core::FeatureDescriptor& descriptor : definition.descriptors()) {
            registry.registerFeature(descriptor, [descriptor](const core::FeatureCreateContext&) {
                return std::make_unique<StubFeature>(descriptor);
            });
        }
        return registry;
    }

    [[nodiscard]] core::RenderDeviceCapabilities rasterCapabilities() {
        return {.supported = {core::RenderCapability::Graphics, core::RenderCapability::Compute,
                              core::RenderCapability::DynamicRendering},
                .maxFramesInFlight = 2};
    }

    void testRasterRecipeUsesTypedDataDag() {
        const pipelines::DefaultRenderPipelineDefinition definition =
            pipelines::makeDefaultRenderPipeline(pipelines::DefaultRenderPipelineKind::Raster);
        const core::RenderFeatureRegistry registry = registryFor(definition);
        const core::ResolvedRenderPipeline resolved =
            core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), rasterCapabilities());

        const std::vector expected = {
            pipelines::feature_ids::atmosphere(),    pipelines::feature_ids::shadow(),
            pipelines::feature_ids::rasterSurface(), pipelines::feature_ids::globalIllumination(),
            pipelines::feature_ids::denoising(),     pipelines::feature_ids::lightingComposite(),
            pipelines::feature_ids::temporalAa(),    pipelines::feature_ids::toneMapping(),
            pipelines::feature_ids::presentation(),
        };
        require(std::ranges::equal(resolved.executionOrder(), expected),
                "Raster recipe must be ordered by typed producer/consumer contracts.");
        require(definition.descriptor(pipelines::feature_ids::rasterSurface()).outputs ==
                    std::vector{core::frame_data::rasterSurface()},
                "Raster surface Feature must publish the typed RasterSurfaceData contract.");
        require(!registry.contains(pipelines::feature_ids::hybridSurface()),
                "Raster registration must not contain the Hybrid surface module.");
    }

    void testHybridCapabilityGateAndTopology() {
        const pipelines::DefaultRenderPipelineDefinition definition =
            pipelines::makeDefaultRenderPipeline(pipelines::DefaultRenderPipelineKind::Hybrid);
        const core::RenderFeatureRegistry registry = registryFor(definition);
        requireThrows<std::runtime_error>(
            [&] {
                static_cast<void>(
                    core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), rasterCapabilities()));
            },
            "Hybrid recipe must be rejected when acceleration-structure capabilities are unavailable.");

        core::RenderDeviceCapabilities capabilities = rasterCapabilities();
        capabilities.supported.add(core::RenderCapability::AccelerationStructure)
            .add(core::RenderCapability::RayTracingPipeline);
        const core::ResolvedRenderPipeline resolved =
            core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), capabilities);
        require(std::ranges::find(resolved.executionOrder(), pipelines::feature_ids::hybridSurface()) !=
                    resolved.executionOrder().end(),
                "Hybrid recipe must activate its RT surface producer when capabilities are present.");
        require(!registry.contains(pipelines::feature_ids::shadow()) &&
                    !registry.contains(pipelines::feature_ids::rasterSurface()),
                "Hybrid registration must not instantiate Raster-only modules.");
        require(definition.descriptor(pipelines::feature_ids::hybridSurface()).outputs ==
                    std::vector{core::frame_data::gpuScene(), core::frame_data::rtSurface()},
                "Hybrid surface Feature must publish GPU Scene and RT surface contracts.");
    }

} // namespace

int main() {
    try {
        testRasterRecipeUsesTypedDataDag();
        testHybridCapabilityGateAndTopology();
        std::cout << "Default render pipeline tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Default render pipeline test failed: " << exception.what() << '\n';
        return 1;
    }
}

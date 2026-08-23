#include "render/gi/raytracing/SharcRadianceCache.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Callable> void requireInvalidArgument(Callable&& callable, const char* message) {
        try {
            callable();
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error(message);
    }

    void testDefaultConfigAndVendorBufferAbi() {
        using namespace lumin::render::gi;
        const SharcRadianceCacheConfig config;
        require(validateSharcRadianceCacheConfig(config), "The conservative default SHARC config must be valid.");
        require(config.capacity == (1U << 20U) && (config.capacity & (config.capacity - 1U)) == 0,
                "The default cache capacity must be a conservative power of two.");
        require(SharcBufferStrides::hashEntry == 8 && SharcBufferStrides::accumulation == 16 &&
                    SharcBufferStrides::resolved == 16 && SharcBufferStrides::lock == 4,
                "SHARC buffer strides must match vendor 1.6.5 shader structs.");

        const nvrhi::BufferDesc hash =
            detail::makeSharcBufferDesc(detail::SharcBufferKind::HashEntries, config.capacity, "hash");
        const nvrhi::BufferDesc accumulation =
            detail::makeSharcBufferDesc(detail::SharcBufferKind::Accumulation, config.capacity, "accumulation");
        const nvrhi::BufferDesc resolved =
            detail::makeSharcBufferDesc(detail::SharcBufferKind::Resolved, config.capacity, "resolved");
        const nvrhi::BufferDesc lock =
            detail::makeSharcBufferDesc(detail::SharcBufferKind::Lock, config.capacity, "lock");
        require(hash.byteSize == std::uint64_t(config.capacity) * 8 && hash.structStride == 8 && hash.canHaveUAVs &&
                    accumulation.structStride == 16 && resolved.structStride == 16 &&
                    lock.byteSize == std::uint64_t(config.capacity) * 4 && lock.structStride == 4 && lock.canHaveUAVs,
                "Hash, accumulation, resolved, and 32-bit lock buffers must use exact vendor ABI.");

        const nvrhi::BufferDesc readback =
            detail::makeSharcBufferDesc(detail::SharcBufferKind::StatisticsReadback, config.capacity, "readback");
        require(readback.byteSize == SharcStatisticsLayout::byteSize &&
                    readback.cpuAccess == nvrhi::CpuAccessMode::Read && !readback.canHaveUAVs &&
                    readback.initialState == nvrhi::ResourceStates::CopyDest,
                "Statistics readback must be a CPU-readable copy destination.");
    }

    void testRejectsUnsafeConfiguration() {
        using namespace lumin::render::gi;
        SharcRadianceCacheConfig config;
        config.capacity = 1000;
        require(!validateSharcRadianceCacheConfig(config), "Non-power-of-two capacity must be rejected.");
        config = {};
        config.staleFrameCount = 7;
        require(!validateSharcRadianceCacheConfig(config), "Vendor-unsafe stale windows must be rejected.");
        config = {};
        config.responsiveAccumulationFrameCount = config.accumulationFrameCount + 1;
        require(!validateSharcRadianceCacheConfig(config), "Responsive accumulation cannot exceed steady state.");
        config = {};
        config.logarithmBase = 1.0F;
        require(!validateSharcRadianceCacheConfig(config), "The logarithm base must be greater than one.");
    }

    void testDescriptorAndDispatchContracts() {
        using namespace lumin::render::gi;
        constexpr std::uint32_t geometryCapacity = 43;
        constexpr std::uint32_t materialTextureCapacity = 7;
        const nvrhi::BindingLayoutDesc update =
            detail::makeSharcUpdateBindingLayoutDesc(geometryCapacity, materialTextureCapacity);
        require(update.visibility == nvrhi::ShaderType::AllRayTracing && update.bindings.size() == 18,
                "SHARC update set 0 must expose scene, cache, constants, and material textures.");
        require(update.bindings[4].type == nvrhi::ResourceType::StructuredBuffer_SRV &&
                    update.bindings[4].getArraySize() == geometryCapacity &&
                    update.bindings[5].getArraySize() == geometryCapacity,
                "SHARC update bindless geometry arrays must share the configured capacity.");
        for (std::size_t index = 8; index <= 12; ++index) {
            require(update.bindings[index].type == nvrhi::ResourceType::StructuredBuffer_UAV,
                    "SHARC cache and statistics bindings must be storage buffers.");
        }
        require(update.bindings[13].type == nvrhi::ResourceType::ConstantBuffer,
                "SHARC update binding 13 must contain SharcGpuConstants.");
        require(update.bindings[14].slot == 14 && update.bindings[14].getArraySize() == materialTextureCapacity &&
                    update.bindings[15].slot == 15 && update.bindings[15].getArraySize() == materialTextureCapacity &&
                    update.bindings[16].slot == 16 && update.bindings[16].type == nvrhi::ResourceType::Sampler,
                "SHARC update bindings 14-16 must expose both material arrays and their sampler.");
        require(update.bindings[17].slot == 17 && update.bindings[17].type == nvrhi::ResourceType::Texture_SRV,
                "SHARC update binding 17 must expose primary material IDs for Cook-Torrance throughput.");

        const nvrhi::BindingLayoutDesc resolve = detail::makeSharcResolveBindingLayoutDesc();
        require(resolve.visibility == nvrhi::ShaderType::Compute && resolve.bindings.size() == 6,
                "SHARC resolve must bind four cache buffers, statistics, and constants.");
        require(detail::makeSharcUpdateDispatchSize(1920, 1080, 5).width == 384 &&
                    detail::makeSharcUpdateDispatchSize(1920, 1080, 5).height == 216,
                "A 5x5 sparse update must trace exactly four percent of a divisible extent.");
        require(detail::makeSharcUpdateDispatchSize(13, 11, 5).width == 3 &&
                    detail::makeSharcUpdateDispatchSize(13, 11, 5).height == 3,
                "Sparse dispatch dimensions must round up partial tiles.");
        require(detail::makeSharcResolveDispatchSize(1U << 20U) == 4096,
                "Resolve must dispatch one 256-thread lane per cache entry.");
        requireInvalidArgument(
            [] {
                (void)detail::makeSharcUpdateBindingLayoutDesc(0);
            },
            "A zero-sized geometry descriptor table must be rejected.");
    }

    void testHistoryCommitDiscardAndResponsiveDecay() {
        using namespace lumin::render::gi;
        const SharcRadianceCacheConfig config;
        SharcHistoryTracker history(config);
        const glm::vec4 firstCamera(1.0F, 2.0F, 3.0F, 1.0F);
        const SharcHistoryPlan first = history.beginFrame(firstCamera, {});
        require(first.isValid() && first.invalidation == SharcInvalidationMode::FullReset &&
                    first.previousCameraPosition == firstCamera,
                "The first SHARC frame must fully reset and suppress stale camera reprojection.");
        history.commitSubmittedFrame();
        require(history.initialized() && history.submittedFrameCount() == 1 &&
                    history.previousCameraPosition() == firstCamera,
                "Only commit may publish the first SHARC history state.");

        const glm::vec4 secondCamera(2.0F, 3.0F, 4.0F, 1.0F);
        SharcInvalidationInputs materialChange;
        materialChange.materialChanged = true;
        const SharcHistoryPlan discarded = history.beginFrame(secondCamera, materialChange);
        require(discarded.invalidation == SharcInvalidationMode::ResponsiveDecay &&
                    discarded.accumulationFrameCount == config.responsiveAccumulationFrameCount,
                "Material changes must select the responsive accumulation window.");
        history.discardPendingFrame();
        require(history.submittedFrameCount() == 1 && history.responsiveFramesRemaining() == 0 &&
                    history.previousCameraPosition() == firstCamera,
                "Discard must preserve the last submitted SHARC state for retry.");

        const SharcHistoryPlan retry = history.beginFrame(secondCamera, materialChange);
        history.commitSubmittedFrame();
        require(retry.resolveFrameIndex == 1 && history.submittedFrameCount() == 2 &&
                    history.responsiveFramesRemaining() == config.responsiveFrameCount - 1 &&
                    history.previousCameraPosition() == secondCamera,
                "A successful retry must advance sequence, camera, and responsive decay exactly once.");

        const SharcHistoryPlan decay = history.beginFrame(secondCamera, {});
        require(decay.invalidation == SharcInvalidationMode::ResponsiveDecay &&
                    decay.responsiveFramesAfterSubmit == config.responsiveFrameCount - 2,
                "Responsive decay must continue for a configured number of successful submissions.");
        history.commitSubmittedFrame();

        SharcInvalidationInputs geometryChange;
        geometryChange.geometryChanged = true;
        const glm::vec4 resetCamera(20.0F, 3.0F, 4.0F, 1.0F);
        const SharcHistoryPlan reset = history.beginFrame(resetCamera, geometryChange);
        require(reset.invalidation == SharcInvalidationMode::FullReset && reset.previousCameraPosition == resetCamera &&
                    reset.responsiveFramesAfterSubmit == 0,
                "Geometry changes must fully reset cache data and camera reprojection history.");
        history.discardPendingFrame();
    }

    void testGpuConstantAbi() {
        using namespace lumin::render::gi;
        const SharcRadianceCacheConfig config;
        SharcHistoryTracker history(config);
        SharcFrameParameters frame;
        frame.cameraPosition = glm::vec4(3.0F, 4.0F, 5.0F, 1.0F);
        frame.toSunWorld = glm::vec4(0.0F, 1.0F, 0.0F, 0.0F);
        frame.sunIrradiance = glm::vec4(8.0F, 7.0F, 6.0F, 2.0F);
        frame.renderWidth = 1600;
        frame.renderHeight = 900;
        const SharcHistoryPlan plan = history.beginFrame(frame.cameraPosition, {});
        const SharcGpuConstants constants = buildSharcGpuConstants(config, plan, frame);
        require(sizeof(constants) == 128 && constants.cameraPositionSceneScale.w == config.sceneScale &&
                    constants.previousCameraPositionLogarithmBase.w == config.logarithmBase &&
                    constants.toSunWorldRadianceScale.w == config.radianceScale &&
                    constants.cacheParameters.x == config.capacity &&
                    constants.cacheParameters.y == config.accumulationFrameCount &&
                    constants.renderParameters == glm::uvec4(1600U, 900U, config.sparseTileSize, 0U),
                "SHARC CPU constants must match the 128-byte shader ABI.");
        history.discardPendingFrame();
    }

} // namespace

int main() {
    try {
        testDefaultConfigAndVendorBufferAbi();
        testRejectsUnsafeConfiguration();
        testDescriptorAndDispatchContracts();
        testHistoryCommitDiscardAndResponsiveDecay();
        testGpuConstantAbi();
        std::puts("SharcRadianceCache PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SharcRadianceCache FAIL: %s\n", error.what());
        return 1;
    }
}

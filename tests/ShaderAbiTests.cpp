#include "render/ModelRenderer.hpp"
#include "render/features/postfx/PostFxResources.hpp"
#include "render/gi/raytracing/GiComposite.hpp"
#include "render/gi/raytracing/RtDiNrdInputs.hpp"
#include "render/gi/raytracing/SharcIndirectLighting.hpp"
#include "render/gi/raytracing/SharcRadianceCache.hpp"
#include "render/gpu/GpuMaterial.hpp"
#include "render/gpu/GpuSceneResources.hpp"

#include <cstddef>
#include <cstdio>
#include <type_traits>

namespace {

    using lumin::render::ObjectData;
    using lumin::render::PostProcessUniforms;

    // CPU 结构必须逐字段匹配 shaders/include/PostProcessUniforms.slang 的反射布局。
    static_assert(std::is_standard_layout_v<PostProcessUniforms>);
    static_assert(sizeof(PostProcessUniforms) == 528);
    static_assert(alignof(PostProcessUniforms) == 16);
    static_assert(offsetof(PostProcessUniforms, inverseViewProjection) == 0);
    static_assert(offsetof(PostProcessUniforms, viewProjection) == 64);
    static_assert(offsetof(PostProcessUniforms, cascadeViewProjections) == 128);
    static_assert(offsetof(PostProcessUniforms, cascadeSplits) == 384);
    static_assert(offsetof(PostProcessUniforms, cameraPosition) == 400);
    static_assert(offsetof(PostProcessUniforms, cameraForward) == 416);
    static_assert(offsetof(PostProcessUniforms, lightDirection) == 432);
    static_assert(offsetof(PostProcessUniforms, renderSize) == 448);
    static_assert(offsetof(PostProcessUniforms, renderOptions) == 464);
    static_assert(offsetof(PostProcessUniforms, tonemapOptions) == 480);
    static_assert(offsetof(PostProcessUniforms, ambientOcclusionOptions) == 496);
    static_assert(offsetof(PostProcessUniforms, temporalOptions) == 512);

    static_assert(sizeof(ObjectData) == 240 && alignof(ObjectData) == 16);
    static_assert(offsetof(ObjectData, model) == 0 && offsetof(ObjectData, previousModel) == 64 &&
                  offsetof(ObjectData, normalMatrix) == 128 && offsetof(ObjectData, baseColorMetallic) == 192 &&
                  offsetof(ObjectData, materialParameters) == 208 && offsetof(ObjectData, metadata) == 224);

    using lumin::render::gi::GiCompositeConstants;
    using lumin::render::gi::RtDiNrdInputsConstants;
    using lumin::render::gi::SharcGpuConstants;
    using lumin::render::gi::SharcIndirectLightingConstants;
    using lumin::render::gpu::GpuInstanceData;
    using lumin::render::gpu::GpuLightData;
    using lumin::render::gpu::GpuMaterialData;
    using lumin::render::gpu::GpuPackedVertex;

    static_assert(sizeof(GpuPackedVertex) == 32 && offsetof(GpuPackedVertex, position) == 0 &&
                  offsetof(GpuPackedVertex, normal) == 16);
    static_assert(sizeof(GpuInstanceData) == 144 && offsetof(GpuInstanceData, model) == 0 &&
                  offsetof(GpuInstanceData, normalMatrix) == 64 && offsetof(GpuInstanceData, metadata) == 128);
    static_assert(sizeof(GpuMaterialData) == 64 && offsetof(GpuMaterialData, baseColorMetallic) == 0 &&
                  offsetof(GpuMaterialData, specularColorShininess) == 16 &&
                  offsetof(GpuMaterialData, surfaceParameters) == 32 && offsetof(GpuMaterialData, metadata) == 48);
    static_assert(sizeof(GpuLightData) == 64 && offsetof(GpuLightData, positionRange) == 0 &&
                  offsetof(GpuLightData, directionCosOuter) == 16 && offsetof(GpuLightData, colorIntensity) == 32 &&
                  offsetof(GpuLightData, parameters) == 48);
    static_assert(sizeof(SharcIndirectLightingConstants) == 80 &&
                  offsetof(SharcIndirectLightingConstants, cameraPosition) == 0 &&
                  offsetof(SharcIndirectLightingConstants, cameraForward) == 16 &&
                  offsetof(SharcIndirectLightingConstants, renderParameters) == 32 &&
                  offsetof(SharcIndirectLightingConstants, traceParameters) == 48 &&
                  offsetof(SharcIndirectLightingConstants, samplingParameters) == 64);
    static_assert(sizeof(GiCompositeConstants) == 48 && alignof(GiCompositeConstants) == 16 &&
                  offsetof(GiCompositeConstants, cameraPosition) == 0 &&
                  offsetof(GiCompositeConstants, renderInfo) == 16 && offsetof(GiCompositeConstants, options) == 32);
    static_assert(sizeof(RtDiNrdInputsConstants) == 48 && alignof(RtDiNrdInputsConstants) == 16 &&
                  offsetof(RtDiNrdInputsConstants, cameraPosition) == 0 &&
                  offsetof(RtDiNrdInputsConstants, renderParameters) == 16 &&
                  offsetof(RtDiNrdInputsConstants, renderInfo) == 32);
    static_assert(sizeof(SharcGpuConstants) == 128 && alignof(SharcGpuConstants) == 16 &&
                  offsetof(SharcGpuConstants, cameraPositionSceneScale) == 0 &&
                  offsetof(SharcGpuConstants, previousCameraPositionLogarithmBase) == 16 &&
                  offsetof(SharcGpuConstants, toSunWorldRadianceScale) == 32 &&
                  offsetof(SharcGpuConstants, sunIrradiance) == 48 &&
                  offsetof(SharcGpuConstants, traceParameters) == 64 &&
                  offsetof(SharcGpuConstants, cacheParameters) == 80 &&
                  offsetof(SharcGpuConstants, renderParameters) == 96 &&
                  offsetof(SharcGpuConstants, samplingParameters) == 112);
    static_assert(lumin::render::gi::SharcBufferStrides::accumulation == 16 &&
                  lumin::render::gi::SharcBufferStrides::resolved == 16);

} // namespace

int main() {
    std::puts("Shader CPU ABI tests passed: raster, RTDI, SHARC indirect, NRD, and composite layouts match Slang.");
    return 0;
}

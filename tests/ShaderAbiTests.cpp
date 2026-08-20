#include "render/ModelRenderer.hpp"
#include "render/resources/TextureManager.hpp"
#include "render/gi/GiComposite.hpp"
#include "render/gi/RayTracedGi.hpp"
#include "render/gi/SharcRadianceCache.hpp"
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
    static_assert(sizeof(PostProcessUniforms) == 496);
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

    static_assert(sizeof(ObjectData) == 240 && alignof(ObjectData) == 16);
    static_assert(offsetof(ObjectData, model) == 0 && offsetof(ObjectData, previousModel) == 64 &&
                  offsetof(ObjectData, normalMatrix) == 128 && offsetof(ObjectData, baseColorMetallic) == 192 &&
                  offsetof(ObjectData, materialParameters) == 208 && offsetof(ObjectData, metadata) == 224);

    using lumin::render::gi::GiCompositeConstants;
    using lumin::render::gi::RayTracedGiConstants;
    using lumin::render::gi::SharcGpuConstants;
    using lumin::render::gpu::GpuInstanceData;
    using lumin::render::gpu::GpuMaterialData;
    using lumin::render::gpu::GpuPackedVertex;

    static_assert(sizeof(GpuPackedVertex) == 32 && offsetof(GpuPackedVertex, position) == 0 &&
                  offsetof(GpuPackedVertex, normal) == 16);
    static_assert(sizeof(GpuInstanceData) == 144 && offsetof(GpuInstanceData, model) == 0 &&
                  offsetof(GpuInstanceData, normalMatrix) == 64 && offsetof(GpuInstanceData, metadata) == 128);
    static_assert(sizeof(GpuMaterialData) == 64 && offsetof(GpuMaterialData, baseColorMetallic) == 0 &&
                  offsetof(GpuMaterialData, specularColorShininess) == 16 &&
                  offsetof(GpuMaterialData, surfaceParameters) == 32 && offsetof(GpuMaterialData, metadata) == 48);
    static_assert(sizeof(RayTracedGiConstants) == 96 && offsetof(RayTracedGiConstants, cameraPosition) == 0 &&
                  offsetof(RayTracedGiConstants, cameraForward) == 16 &&
                  offsetof(RayTracedGiConstants, toSunWorld) == 32 &&
                  offsetof(RayTracedGiConstants, sunRadiance) == 48 &&
                  offsetof(RayTracedGiConstants, renderSize) == 64 &&
                  offsetof(RayTracedGiConstants, traceParameters) == 80);
    static_assert(sizeof(GiCompositeConstants) == 32 && alignof(GiCompositeConstants) == 16 &&
                  offsetof(GiCompositeConstants, cameraPosition) == 0 &&
                  offsetof(GiCompositeConstants, renderInfo) == 16);
    static_assert(sizeof(SharcGpuConstants) == 128 && alignof(SharcGpuConstants) == 16 &&
                  offsetof(SharcGpuConstants, cameraPositionSceneScale) == 0 &&
                  offsetof(SharcGpuConstants, previousCameraPositionLogarithmBase) == 16 &&
                  offsetof(SharcGpuConstants, toSunWorldRadianceScale) == 32 &&
                  offsetof(SharcGpuConstants, sunRadiance) == 48 &&
                  offsetof(SharcGpuConstants, traceParameters) == 64 &&
                  offsetof(SharcGpuConstants, cacheParameters) == 80 &&
                  offsetof(SharcGpuConstants, renderParameters) == 96 &&
                  offsetof(SharcGpuConstants, reserved) == 112);
    static_assert(lumin::render::gi::SharcBufferStrides::accumulation == 16 &&
                  lumin::render::gi::SharcBufferStrides::resolved == 16);

} // namespace

int main() {
    std::puts("Shader CPU ABI tests passed: raster, RT GI, SHARC, and composite layouts match Slang reflection.");
    return 0;
}

#include "GpuScene.slang"
#include "PhysicalLighting.slang"
#include "NRD.hlsli"

struct GiCompositeConstants {
    float4 cameraPosition;
    uint4 renderInfo;
};

[[vk::binding(0, 0)]] Texture2D<float4> denoisedDiffuseRadianceHitDistance;
[[vk::binding(1, 0)]] Texture2D<float4> denoisedSpecularRadianceHitDistance;
[[vk::binding(2, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(3, 0)]] Texture2D<float4> normalRoughnessTexture;
[[vk::binding(4, 0)]] Texture2D<float4> albedoMetallicTexture;
[[vk::binding(5, 0)]] Texture2D<uint> materialIdTexture;
[[vk::binding(6, 0)]] StructuredBuffer<GpuMaterialData> materials;
[[vk::binding(7, 0)]] ConstantBuffer<GiCompositeConstants> frame;
[[vk::binding(8, 0)]] RWTexture2D<float4> globalIlluminationOutput;

static const float4 kNeutralOutput = float4(0.0, 0.0, 0.0, 1.0);

float3 unpackReblurRadiance(float4 packedSignal) {
    // Raw RT GI 与 NRD 输出都使用 REBLUR 的 YCoCg signal contract。
    return max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packedSignal).rgb, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= frame.renderInfo.xy)) {
        return;
    }

    float4 position = positionTexture.Load(int3(pixel, 0));
    uint materialId = materialIdTexture.Load(int3(pixel, 0));
    // RT surface 的 position.w 是 hit distance，背景为负值；近距离命中仍然是有效几何。
    if (position.w <= 0.0 || materialId == frame.renderInfo.w || materialId >= frame.renderInfo.z) {
        // 背景和失效材质都恢复 packed GI 的中性值，不能把旧帧 UAV 内容泄漏到本帧。
        globalIlluminationOutput[pixel] = kNeutralOutput;
        return;
    }

    float3 diffuseRadiance =
        unpackReblurRadiance(denoisedDiffuseRadianceHitDistance.Load(int3(pixel, 0)));
    float3 specularRadiance =
        unpackReblurRadiance(denoisedSpecularRadianceHitDistance.Load(int3(pixel, 0)));
    float4 normalRoughness = normalRoughnessTexture.Load(int3(pixel, 0));
    float3 normal = luminPhysicalSafeNormalize(normalRoughness.xyz, float3(0.0, 0.0, 1.0));
    GpuMaterialData primaryMaterial = materials[materialId];
    primaryMaterial.baseColorMetallic = albedoMetallicTexture.Load(int3(pixel, 0));
    primaryMaterial.surfaceParameters.x = normalRoughness.w;
    LuminCookTorranceMaterial cookTorranceMaterial = luminMakeCookTorranceMaterial(primaryMaterial);
    float3 toView = luminPhysicalSafeNormalize(frame.cameraPosition.xyz - position.xyz, normal);
    float3 diffuseMaterialFactor;
    float3 specularMaterialFactor;
    NRD_MaterialFactors(normal, toView, cookTorranceMaterial.diffuseColor,
                        cookTorranceMaterial.reflectanceAtNormal, cookTorranceMaterial.roughness,
                        diffuseMaterialFactor, specularMaterialFactor);

    // 仅在 REBLUR 之后恢复主表面材质，避免空间滤波把 albedo、metallic 与 Fresnel 细节一起模糊。
    float3 outgoingRadiance = diffuseRadiance * diffuseMaterialFactor +
                              specularRadiance * specularMaterialFactor;
    globalIlluminationOutput[pixel] = float4(luminClampFp16Radiance(outgoingRadiance), 0.0);
}

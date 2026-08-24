import GpuScene;
import PhysicalLighting;
#include "NRD.hlsli"

struct RtDiNrdInputsConstants {
    float4 cameraPosition;
    float4 renderParameters;
    uint4 renderInfo;
};

[[vk::binding(0, 0)]] Texture2D<float4> rawDiffuseRadianceHitT;
[[vk::binding(1, 0)]] Texture2D<float4> rawSpecularRadianceHitT;
[[vk::binding(2, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(3, 0)]] Texture2D<float4> normalRoughnessTexture;
[[vk::binding(4, 0)]] Texture2D<float4> albedoMetallicTexture;
[[vk::binding(5, 0)]] Texture2D<uint> materialIdTexture;
[[vk::binding(6, 0)]] Texture2D<float> viewZTexture;
[[vk::binding(7, 0)]] Texture2D<float2> motionTexture;
[[vk::binding(8, 0)]] StructuredBuffer<GpuMaterialData> materials;
[[vk::binding(9, 0)]] ConstantBuffer<RtDiNrdInputsConstants> frame;
[[vk::binding(10, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> diffuseRadianceHitDistance;
[[vk::binding(11, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> specularRadianceHitDistance;
[[vk::binding(12, 0), vk::image_format("r32f")]] RWTexture2D<float> denoiserViewZ;
[[vk::binding(13, 0), vk::image_format("rgb10_a2")]] RWTexture2D<float4> denoiserNormalRoughness;
[[vk::binding(14, 0), vk::image_format("rg16f")]] RWTexture2D<float2> denoiserMotion;

[shader("compute")]
[numthreads(8, 8, 1)]
void prepareMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= frame.renderInfo.xy)) {
        return;
    }

    denoiserMotion[pixel] = motionTexture.Load(int3(pixel, 0)) + frame.renderParameters.xy;
    float4 position = positionTexture.Load(int3(pixel, 0));
    uint materialId = materialIdTexture.Load(int3(pixel, 0));
    if (position.w <= 0.0 || materialId == frame.renderInfo.w || materialId >= frame.renderInfo.z) {
        diffuseRadianceHitDistance[pixel] = float4(0.0);
        specularRadianceHitDistance[pixel] = float4(0.0);
        denoiserViewZ[pixel] = frame.renderParameters.z;
        denoiserNormalRoughness[pixel] = float4(0.0);
        return;
    }

    float4 normalRoughness = normalRoughnessTexture.Load(int3(pixel, 0));
    float3 normal = luminPhysicalSafeNormalize(normalRoughness.xyz, float3(0.0, 0.0, 1.0));
    GpuMaterialData material = materials[materialId];
    material.baseColorMetallic = albedoMetallicTexture.Load(int3(pixel, 0));
    material.surfaceParameters.x = clamp(normalRoughness.w, 0.04, 1.0);
    LuminCookTorranceMaterial physicalMaterial = luminMakeCookTorranceMaterial(material);
    float3 toView = luminPhysicalSafeNormalize(frame.cameraPosition.xyz - position.xyz, normal);
    float3 diffuseMaterialFactor;
    float3 specularMaterialFactor;
    NRD_MaterialFactors(normal, toView, physicalMaterial.diffuseColor, physicalMaterial.reflectanceAtNormal,
                        physicalMaterial.roughness, diffuseMaterialFactor, specularMaterialFactor);

    float primaryViewZ = max(viewZTexture.Load(int3(pixel, 0)), 0.0);
    float4 rawDiffuse = rawDiffuseRadianceHitT.Load(int3(pixel, 0));
    float4 rawSpecular = rawSpecularRadianceHitT.Load(int3(pixel, 0));
    float diffuseNormHitDistance = rawDiffuse.a > 0.0
        ? REBLUR_FrontEnd_GetNormHitDist(
              rawDiffuse.a, primaryViewZ, float3(3.0, 0.1, 20.0), physicalMaterial.roughness)
        : 0.0;
    float specularNormHitDistance = rawSpecular.a > 0.0
        ? REBLUR_FrontEnd_GetNormHitDist(
              rawSpecular.a, primaryViewZ, float3(3.0, 0.1, 20.0), physicalMaterial.roughness)
        : 0.0;
    float3 demodulatedDiffuse = max(rawDiffuse.rgb, 0.0) / max(diffuseMaterialFactor, float3(1.0e-4));
    float3 demodulatedSpecular = max(rawSpecular.rgb, 0.0) / max(specularMaterialFactor, float3(1.0e-4));

    diffuseRadianceHitDistance[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        luminClampFp16Radiance(demodulatedDiffuse), diffuseNormHitDistance, true);
    specularRadianceHitDistance[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        luminClampFp16Radiance(demodulatedSpecular), specularNormHitDistance, true);
    denoiserViewZ[pixel] = primaryViewZ;
    denoiserNormalRoughness[pixel] =
        NRD_FrontEnd_PackNormalAndRoughness(normal, physicalMaterial.roughness, 0.0);
}

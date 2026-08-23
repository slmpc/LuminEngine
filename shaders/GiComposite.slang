#include "GpuScene.slang"
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

static const uint kBlinnPhongSurfaceModel = 1u;
static const float4 kNeutralOutput = float4(0.0, 0.0, 0.0, 1.0);

float3 safeNormalize(float3 value, float3 fallback) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 1e-12 ? value * rsqrt(lengthSquared) : fallback;
}

float3 fresnelSchlick(float normalDotView, float3 reflectanceAtNormal) {
    return reflectanceAtNormal + (1.0 - reflectanceAtNormal) * pow(1.0 - saturate(normalDotView), 5.0);
}

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

    GpuMaterialData material = materials[materialId];
    float3 diffuseRadiance =
        unpackReblurRadiance(denoisedDiffuseRadianceHitDistance.Load(int3(pixel, 0)));
    float3 specularRadiance =
        unpackReblurRadiance(denoisedSpecularRadianceHitDistance.Load(int3(pixel, 0)));
    float4 albedoMetallic = albedoMetallicTexture.Load(int3(pixel, 0));
    float3 albedo = saturate(albedoMetallic.rgb);
    float metallic = saturate(albedoMetallic.a);

    float3 indirectRadiance;
    float3 ambientFloor;
    if (material.metadata.x == kBlinnPhongSurfaceModel) {
        // 路径采样已包含 cosine/pdf 权重；这里只恢复主表面的漫反射与传统镜面颜色。
        float3 specularColor = saturate(material.specularColorShininess.rgb);
        indirectRadiance = diffuseRadiance * albedo + specularRadiance * specularColor;
        ambientFloor = albedo * 0.12 + specularColor * 0.04;
    } else {
        float3 normal = safeNormalize(normalRoughnessTexture.Load(int3(pixel, 0)).xyz,
                                      float3(0.0, 0.0, 1.0));
        float3 toView = safeNormalize(frame.cameraPosition.xyz - position.xyz, normal);
        float normalDotView = saturate(dot(normal, toView));
        float3 reflectanceAtNormal = lerp(float3(0.04), albedo, metallic);
        float3 fresnel = fresnelSchlick(normalDotView, reflectanceAtNormal);
        float3 diffuseWeight = (1.0 - fresnel) * (1.0 - metallic);
        indirectRadiance = diffuseRadiance * albedo * diffuseWeight + specularRadiance * fresnel;
        ambientFloor = albedo * (1.0 - metallic) * 0.12 + reflectanceAtNormal * 0.04;
    }

    // 单样本 raw RT 与尚未收敛的 SHARC 可能暂时返回近零能量。取既有材质环境项作为下限，避免切换拓扑后
    // 阴影区域骤黑；较强的真实反弹光原样保留，不与该下限重复相加。
    globalIlluminationOutput[pixel] = float4(max(indirectRadiance, ambientFloor), 0.0);
}

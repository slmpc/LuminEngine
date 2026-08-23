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

    // RTGI 已在采样点应用 Cook-Torrance BRDF * cos(theta) / PDF；这里只合并去噪后的两个出射辐亮度信号。
    globalIlluminationOutput[pixel] =
        float4(luminClampFp16Radiance(diffuseRadiance + specularRadiance), 0.0);
}

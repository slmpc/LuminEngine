struct HybridLightingCompositeConstants {
    uint4 renderInfo;
};

static const float kFp16Max = 65504.0;

[[vk::binding(0, 0)]] Texture2D<float4> directRadiance;
[[vk::binding(1, 0)]] Texture2D<float4> indirectRadiance;
[[vk::binding(2, 0)]] RWTexture2D<float4> lightingOutput;
[[vk::binding(3, 0)]] ConstantBuffer<HybridLightingCompositeConstants> frame;

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= frame.renderInfo.xy)) {
        return;
    }

    float3 direct = max(directRadiance.Load(int3(pixel, 0)).rgb, 0.0);
    if (frame.renderInfo.z == 0u) {
        direct += max(indirectRadiance.Load(int3(pixel, 0)).rgb, 0.0);
    } else if (frame.renderInfo.z == 2u) {
        direct *= saturate(indirectRadiance.Load(int3(pixel, 0)).a);
    }
    // direct/indirect 各自有效仍可能在相加时超过 FP16，最终 scene-radiance 写入必须保持有限值。
    lightingOutput[pixel] = float4(min(direct, float3(kFp16Max)), 1.0);
}

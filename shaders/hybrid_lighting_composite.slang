struct HybridLightingCompositeConstants {
    uint4 renderInfo;
};

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
    float4 packedIndirect = indirectRadiance.Load(int3(pixel, 0));
    float3 indirect = 0.0;
    if (frame.renderInfo.z == 0u) {
        indirect = max(packedIndirect.rgb, 0.0);
    } else if (frame.renderInfo.z == 2u) {
        direct *= saturate(packedIndirect.a);
    }
    lightingOutput[pixel] = float4(direct + indirect, 1.0);
}

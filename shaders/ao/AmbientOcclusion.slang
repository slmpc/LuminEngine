#include "AoCommon.slang"
#include "Ssao.slang"
#include "Hbao.slang"
#include "Gtao.slang"

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 centerSample = positionTexture.SampleLevel(renderSampler, input.uv, 0.0);
    // Raster position.w=1，RT surface position.w 为正 hit distance，两个路径都以非正值表示背景。
    if (centerSample.w <= 0.0 || frame.renderOptions.y < 0.5) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 center = centerSample.xyz;
    float3 normal = normalize(normalTexture.SampleLevel(renderSampler, input.uv, 0.0).xyz);
    float radius = max(frame.ambientOcclusionOptions.y, 0.05);
    float strength = max(frame.ambientOcclusionOptions.z, 0.0);
    float bias = clamp(frame.ambientOcclusionOptions.w, 0.0, 0.5);
    float2 pixelPosition = input.position.xy;
    // AO 半径以像素计量，但纹理采样偏移必须换算到归一化 UV，像素坐标仅用于稳定的屏幕空间抖动。
    float2 texelSize = frame.renderSize.zw;
    float randomAngle = hash12(pixelPosition) * 6.2831853;
    float algorithm = frame.ambientOcclusionOptions.x;
    float occlusion;
    if (algorithm < 0.5) {
        occlusion = computeSsao(input.uv, texelSize, center, normal, radius, bias, randomAngle);
    } else if (algorithm < 1.5) {
        occlusion = computeHbao(input.uv, pixelPosition, texelSize, center, normal, radius, bias, randomAngle);
    } else {
        occlusion = computeGtao(input.uv, pixelPosition, texelSize, center, normal, radius, bias, randomAngle);
    }
    float ao = saturate(1.0 - occlusion * strength);
    return float4(0.0, 0.0, 0.0, ao);
}

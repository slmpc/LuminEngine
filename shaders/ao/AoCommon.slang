#ifndef LUMIN_AO_COMMON_SLANG
#define LUMIN_AO_COMMON_SLANG

import PostProcessUniforms;

[[vk::binding(0, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(1, 0)]] Texture2D<float4> normalTexture;
[[vk::binding(2, 0)]] SamplerState renderSampler;
[[vk::binding(3, 0)]] ConstantBuffer<PostProcessUniforms> frame;

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VertexOutput makeAmbientOcclusionVertex(uint vertexId) {
    float2 triangleUv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(triangleUv.x * 2.0 - 1.0, 1.0 - triangleUv.y * 2.0, 0.0, 1.0);
    output.uv = triangleUv;
    return output;
}

float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

bool samplePosition(float2 uv, out float3 position) {
    if (any(uv <= 0.0) || any(uv >= 1.0)) {
        position = 0.0;
        return false;
    }
    float4 sampleValue = positionTexture.SampleLevel(renderSampler, uv, 0.0);
    position = sampleValue.xyz;
    return sampleValue.w > 0.0;
}

float projectedRadiusPixels(float3 center, float worldRadius) {
    float3 forward = normalize(frame.cameraForward.xyz);
    float3 referenceUp = abs(forward.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 cameraRight = normalize(cross(forward, referenceUp));
    float4 centerClip = mul(frame.viewProjection, float4(center, 1.0));
    float4 radiusClip = mul(frame.viewProjection, float4(center + cameraRight * worldRadius, 1.0));
    float2 centerNdc = centerClip.xy / max(abs(centerClip.w), 1e-4);
    float2 radiusNdc = radiusClip.xy / max(abs(radiusClip.w), 1e-4);
    return clamp(length((radiusNdc - centerNdc) * frame.renderSize.xy * 0.5), 3.0, 64.0);
}

float rangeFalloff(float distanceToSample, float radius) {
    float normalizedDistance = saturate(distanceToSample / max(radius, 1e-4));
    return 1.0 - normalizedDistance * normalizedDistance;
}

#endif

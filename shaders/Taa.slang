#include "PostProcessUniforms.slang"

[[vk::binding(0, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(3, 0)]] Texture2D<float2> motionTexture;
[[vk::binding(5, 0)]] Texture2D<float4> lightingTexture;
[[vk::binding(6, 0)]] Texture2D<float4> historyTexture;
[[vk::binding(12, 0)]] SamplerState renderSampler;
[[vk::binding(13, 0)]] ConstantBuffer<PostProcessUniforms> frame;

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    float2 triangleUv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(triangleUv.x * 2.0 - 1.0, 1.0 - triangleUv.y * 2.0, 0.0, 1.0);
    output.uv = triangleUv;
    return output;
}

float luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

bool isGeometrySample(float2 uv) {
    int2 extent = max(int2(frame.renderSize.xy), int2(1));
    int2 pixel = clamp(int2(uv * frame.renderSize.xy), int2(0), extent - 1);
    return positionTexture.Load(int3(pixel, 0)).w > 0.0;
}

float3 sampleStableCurrent(float2 stableUv) {
    float2 jitteredUv = stableUv + frame.temporalOptions.xy;
    // 整张 HDR 输入都在当前抖动坐标生成，统一反向采样才能得到稳定输出。
    return lightingTexture.SampleLevel(renderSampler, jitteredUv, 0.0).rgb;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 jitteredUv = input.uv + frame.temporalOptions.xy;
    bool geometry = isGeometrySample(jitteredUv);
    float3 current = sampleStableCurrent(input.uv);
    if (frame.renderOptions.x < 0.5 || frame.renderOptions.w < 0.5) {
        return float4(current, 1.0);
    }

    // 背景没有可靠 motion，且已在稳定屏幕坐标生成；不累积历史可避免转动相机时的天空拖影。
    if (!geometry) {
        return float4(current, 1.0);
    }

    float2 rawMotion = motionTexture.SampleLevel(renderSampler, jitteredUv, 0.0);
    float2 stableMotion = rawMotion + frame.temporalOptions.xy - frame.temporalOptions.zw;
    float2 previousUv = input.uv + stableMotion;
    if (any(previousUv <= 0.0) || any(previousUv >= 1.0)) {
        return float4(current, 1.0);
    }

    float3 neighborhoodMin = current;
    float3 neighborhoodMax = current;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float3 sampleColor = sampleStableCurrent(input.uv + float2(x, y) * frame.renderSize.zw);
            neighborhoodMin = min(neighborhoodMin, sampleColor);
            neighborhoodMax = max(neighborhoodMax, sampleColor);
        }
    }

    float3 history = historyTexture.SampleLevel(renderSampler, previousUv, 0.0).rgb;
    history = clamp(history, neighborhoodMin, neighborhoodMax);
    float luminanceDelta = abs(luminance(current) - luminance(history));
    float historyWeight = lerp(0.92, 0.70, saturate(luminanceDelta * 2.0));
    return float4(lerp(current, history, historyWeight), 1.0);
}

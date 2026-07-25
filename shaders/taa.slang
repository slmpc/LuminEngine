struct PostProcessUniforms {
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4x4 cascadeViewProjections[4];
    float4 cascadeSplits;
    float4 cameraPosition;
    float4 cameraForward;
    float4 lightDirection;
    float4 renderSize;
    float4 renderOptions;
    float4 tonemapOptions;
};

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

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 current = lightingTexture.SampleLevel(renderSampler, input.uv, 0.0).rgb;
    if (frame.renderOptions.x < 0.5 || frame.renderOptions.w < 0.5) {
        return float4(current, 1.0);
    }

    float2 motion = motionTexture.SampleLevel(renderSampler, input.uv, 0.0);
    float2 previousUv = input.uv - motion;
    if (any(previousUv <= 0.0) || any(previousUv >= 1.0)) {
        return float4(current, 1.0);
    }

    float3 neighborhoodMin = current;
    float3 neighborhoodMax = current;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float3 sampleColor = lightingTexture.SampleLevel(
                renderSampler, input.uv + float2(x, y) * frame.renderSize.zw, 0.0).rgb;
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

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

[[vk::binding(7, 0)]] Texture2D<float4> resolvedTexture;
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
    output.position = float4(triangleUv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = triangleUv;
    return output;
}

float3 acesFilm(float3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 hdrColor = resolvedTexture.SampleLevel(renderSampler, input.uv, 0.0).rgb;
    float3 mapped = acesFilm(hdrColor * max(frame.tonemapOptions.x, 0.01));
    if (frame.tonemapOptions.y < 0.5) {
        mapped = pow(mapped, 1.0 / 2.2);
    }
    return float4(mapped, 1.0);
}

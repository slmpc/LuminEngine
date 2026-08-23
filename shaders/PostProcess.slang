import PostProcessUniforms;
import Fsr1Rcas;

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
    output.position = float4(triangleUv.x * 2.0 - 1.0, 1.0 - triangleUv.y * 2.0, 0.0, 1.0);
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

float3 toDisplayLinear(float3 hdrColor) {
    return acesFilm(hdrColor * max(frame.tonemapOptions.x, 0.01));
}

float3 loadDisplayLinear(int2 pixel) {
    const int2 extent = int2(frame.renderSize.xy);
    const int2 clampedPixel = clamp(pixel, int2(0), extent - 1);
    return toDisplayLinear(resolvedTexture.Load(int3(clampedPixel, 0)).rgb);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    const int2 pixel = int2(input.position.xy);
    float3 mapped = toDisplayLinear(resolvedTexture.SampleLevel(renderSampler, input.uv, 0.0).rgb);
    if (frame.renderOptions.w >= 0.5 && frame.tonemapOptions.z > 0.0) {
        mapped = luminFsr1Rcas(loadDisplayLinear(pixel + int2(0, -1)),
                              loadDisplayLinear(pixel + int2(-1, 0)), mapped,
                              loadDisplayLinear(pixel + int2(1, 0)),
                              loadDisplayLinear(pixel + int2(0, 1)), frame.tonemapOptions.z);
        mapped = saturate(mapped);
    }
    if (frame.tonemapOptions.y < 0.5) {
        mapped = pow(mapped, 1.0 / 2.2);
    }
    return float4(mapped, 1.0);
}

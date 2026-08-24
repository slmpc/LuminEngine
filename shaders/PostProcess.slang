import PostProcessUniforms;
import Fsr1Rcas;

[[vk::binding(14, 0)]] Texture2D<float4> bloomTexture;
[[vk::binding(15, 0)]] Texture2D<float4> autoExposureTexture;
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

float3 agxInset(float3 color) {
    return float3(dot(float3(0.842479062253094, 0.0784335999999992, 0.0792237451477643), color),
                  dot(float3(0.0423282422610123, 0.878468636469772, 0.0791661274605434), color),
                  dot(float3(0.0423756549057051, 0.0784336, 0.879142973793104), color));
}

float3 agxOutset(float3 color) {
    return float3(dot(float3(1.19687900512017, -0.0980208811401368, -0.0990297440797205), color),
                  dot(float3(-0.0528968517574562, 1.15190312990417, -0.0989611768448433), color),
                  dot(float3(-0.0529716355144438, -0.0980434501171241, 1.15107367264116), color));
}

float3 agxDefaultContrastApprox(float3 value) {
    value -= 20.0 / 33.0;
    const bool3 negative = value < 0.0;
    const float3 scale = select(negative, 59.507875, 69.862789);
    const float3 power0 = select(negative, 3.0, 13.0 / 4.0);
    const float3 power1 = select(negative, -1.0 / 3.0, -4.0 / 13.0);
    return 2.0 * value * pow(1.0 + scale * pow(abs(value), power0), power1) + 0.5;
}

float3 agxDisplayLinear(float3 color) {
    color = max(agxInset(max(color, 0.0)), 1.0e-10);
    const float minimumEv = -12.47393;
    const float maximumEv = 4.026069;
    color = clamp(log2(color), minimumEv, maximumEv);
    color = (color - minimumEv) / (maximumEv - minimumEv);
    color = agxDefaultContrastApprox(color);
    // AgX curve output is display encoded; return to linear before RCAS and the render-target transfer function.
    return pow(max(agxOutset(color), 0.0), 2.2);
}

float3 toDisplayLinear(float3 hdrColor) {
    const float autoExposureEv =
        frame.autoExposureOptions.x >= 0.5 ? autoExposureTexture.Load(int3(0, 0, 0)).z : 0.0;
    const float exposureMultiplier =
        max(frame.tonemapOptions.x, 0.0) * exp2(autoExposureEv + frame.autoExposureOptions.y);
    const float3 exposed = hdrColor * exposureMultiplier;
    return frame.tonemapOptions.w >= 0.5 ? agxDisplayLinear(exposed) : acesFilm(exposed);
}

float3 loadDisplayLinear(int2 pixel) {
    const int2 extent = int2(frame.renderSize.xy);
    const int2 clampedPixel = clamp(pixel, int2(0), extent - 1);
    return toDisplayLinear(bloomTexture.Load(int3(clampedPixel, 0)).rgb);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    const int2 pixel = int2(input.position.xy);
    float3 mapped = toDisplayLinear(bloomTexture.SampleLevel(renderSampler, input.uv, 0.0).rgb);
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

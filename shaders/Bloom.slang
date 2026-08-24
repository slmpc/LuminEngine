struct BloomPushConstants {
    // xy: source texel size; z: threshold; w: soft-knee ratio.
    float4 filter;
    // x: intensity; y: radius; z: pass mode; w: reserved.
    float4 controls;
};

[[vk::push_constant]] ConstantBuffer<BloomPushConstants> constants;
[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture;
[[vk::binding(1, 0)]] Texture2D<float4> baseTexture;
[[vk::binding(2, 0)]] SamplerState bloomSampler;

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

float3 sampleSource(float2 uv, float2 offset) {
    return max(sourceTexture.SampleLevel(bloomSampler, uv + offset * constants.filter.xy, 0.0).rgb, 0.0);
}

float3 applySoftThreshold(float3 color) {
    const float threshold = max(constants.filter.z, 0.0);
    const float brightness = max(color.r, max(color.g, color.b));
    const float knee = max(threshold * saturate(constants.filter.w), 1.0e-5);
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1.0e-5);
    const float contribution = max(soft, brightness - threshold) / max(brightness, 1.0e-5);
    return color * saturate(contribution);
}

float3 downsample13Tap(float2 uv) {
    const float3 e = sampleSource(uv, float2(-2.0, -2.0));
    const float3 f = sampleSource(uv, float2(0.0, -2.0));
    const float3 g = sampleSource(uv, float2(2.0, -2.0));
    const float3 a = sampleSource(uv, float2(-1.0, -1.0));
    const float3 b = sampleSource(uv, float2(1.0, -1.0));
    const float3 h = sampleSource(uv, float2(-2.0, 0.0));
    const float3 i = sampleSource(uv, float2(0.0, 0.0));
    const float3 j = sampleSource(uv, float2(2.0, 0.0));
    const float3 c = sampleSource(uv, float2(-1.0, 1.0));
    const float3 d = sampleSource(uv, float2(1.0, 1.0));
    const float3 k = sampleSource(uv, float2(-2.0, 2.0));
    const float3 l = sampleSource(uv, float2(0.0, 2.0));
    const float3 m = sampleSource(uv, float2(2.0, 2.0));

    float3 result = (a + b + c + d) * 0.125;
    result += (e + f + h + i) * 0.03125;
    result += (f + g + i + j) * 0.03125;
    result += (h + i + k + l) * 0.03125;
    result += (i + j + l + m) * 0.03125;
    return result;
}

float3 upsampleTent(float2 uv) {
    const float radius = clamp(constants.controls.y, 0.5, 4.0);
    const float2 texel = constants.filter.xy * radius;
    float3 result = sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(-1.0, -1.0), 0.0).rgb;
    result += sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(1.0, -1.0), 0.0).rgb;
    result += sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(-1.0, 1.0), 0.0).rgb;
    result += sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(1.0, 1.0), 0.0).rgb;
    result *= 0.0625;
    result += sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(0.0, -1.0), 0.0).rgb * 0.125;
    result += sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(-1.0, 0.0), 0.0).rgb * 0.125;
    result += sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(1.0, 0.0), 0.0).rgb * 0.125;
    result += sourceTexture.SampleLevel(bloomSampler, uv + texel * float2(0.0, 1.0), 0.0).rgb * 0.125;
    result += sourceTexture.SampleLevel(bloomSampler, uv, 0.0).rgb * 0.25;
    return max(result, 0.0);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    const uint mode = uint(constants.controls.z + 0.5);
    if (mode <= 1) {
        float3 color = downsample13Tap(input.uv);
        if (mode == 0) {
            color = applySoftThreshold(color);
        }
        return float4(color, 1.0);
    }
    if (mode == 2) {
        const float3 base = max(baseTexture.SampleLevel(bloomSampler, input.uv, 0.0).rgb, 0.0);
        return float4(base + upsampleTent(input.uv), 1.0);
    }
    const float3 scene = max(baseTexture.SampleLevel(bloomSampler, input.uv, 0.0).rgb, 0.0);
    const float3 bloom = max(sourceTexture.SampleLevel(bloomSampler, input.uv, 0.0).rgb, 0.0);
    return float4(scene + bloom * max(constants.controls.x, 0.0), 1.0);
}

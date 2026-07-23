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

[[vk::binding(0, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(1, 0)]] Texture2D<float4> normalTexture;
[[vk::binding(2, 0)]] SamplerState renderSampler;
[[vk::binding(3, 0)]] ConstantBuffer<PostProcessUniforms> frame;

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

float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 centerSample = positionTexture.SampleLevel(renderSampler, input.uv, 0.0);
    if (centerSample.w < 0.5 || frame.renderOptions.y < 0.5) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 center = centerSample.xyz;
    float3 normal = normalize(normalTexture.SampleLevel(renderSampler, input.uv, 0.0).xyz);
    float viewDistance = max(dot(center - frame.cameraPosition.xyz, frame.cameraForward.xyz), 0.1);
    float pixelRadius = clamp(24.0 / sqrt(viewDistance), 4.0, 18.0);
    float worldRadius = clamp(viewDistance * 0.04, 0.12, 2.0);
    float randomAngle = hash12(input.position.xy) * 6.2831853;
    float2x2 rotation = float2x2(cos(randomAngle), -sin(randomAngle),
                                 sin(randomAngle), cos(randomAngle));
    float2 texel = frame.renderSize.zw;
    float occlusion = 0.0;

    [unroll]
    for (uint sampleIndex = 0; sampleIndex < 12; ++sampleIndex) {
        float angle = (float(sampleIndex) + 0.5) * 2.3999632;
        float radius = (float(sampleIndex) + 1.0) / 12.0;
        float2 direction = mul(rotation, float2(cos(angle), sin(angle)));
        float2 sampleUv = input.uv + direction * texel * pixelRadius * radius;
        float4 neighborSample = positionTexture.SampleLevel(renderSampler, sampleUv, 0.0);
        if (neighborSample.w < 0.5) {
            continue;
        }
        float3 delta = neighborSample.xyz - center;
        float distanceToSample = length(delta);
        float horizon = max(dot(normal, delta / max(distanceToSample, 1e-4)) - 0.08, 0.0);
        float rangeWeight = 1.0 - smoothstep(worldRadius * 0.25, worldRadius, distanceToSample);
        occlusion += horizon * rangeWeight;
    }

    float ao = saturate(1.0 - occlusion * (2.2 / 12.0));
    return float4(0.0, 0.0, 0.0, ao);
}

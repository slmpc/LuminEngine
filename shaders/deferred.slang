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
[[vk::binding(2, 0)]] Texture2D<float4> albedoTexture;
[[vk::binding(4, 0)]] Texture2D<float> ambientOcclusionTexture;
[[vk::binding(8, 0)]] Texture2D<float> shadowTexture0;
[[vk::binding(9, 0)]] Texture2D<float> shadowTexture1;
[[vk::binding(10, 0)]] Texture2D<float> shadowTexture2;
[[vk::binding(11, 0)]] Texture2D<float> shadowTexture3;
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

float readShadowDepth(uint cascadeIndex, float2 uv) {
    int2 pixel = clamp(int2(uv * 2048.0), int2(0, 0), int2(2047, 2047));
    switch (cascadeIndex) {
        case 0: return shadowTexture0.Load(int3(pixel, 0));
        case 1: return shadowTexture1.Load(int3(pixel, 0));
        case 2: return shadowTexture2.Load(int3(pixel, 0));
        default: return shadowTexture3.Load(int3(pixel, 0));
    }
}

float calculateShadow(float3 worldPosition, float3 normal, uint cascadeIndex) {
    if (frame.renderOptions.z < 0.5) {
        return 1.0;
    }
    float4 shadowClip = mul(frame.cascadeViewProjections[cascadeIndex], float4(worldPosition, 1.0));
    float3 shadowNdc = shadowClip.xyz / max(shadowClip.w, 1e-5);
    float2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    if (shadowNdc.z <= 0.0 || shadowNdc.z >= 1.0 || any(shadowUv <= 0.0) || any(shadowUv >= 1.0)) {
        return 1.0;
    }

    float3 toLight = normalize(-frame.lightDirection.xyz);
    float bias = max(0.00035 * (1.0 - dot(normal, toLight)), 0.00008);
    float2 texel = float2(1.0 / 2048.0, 1.0 / 2048.0);
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float storedDepth = readShadowDepth(cascadeIndex, shadowUv + float2(x, y) * texel);
            visibility += shadowNdc.z - bias <= storedDepth ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 worldPositionSample = positionTexture.SampleLevel(renderSampler, input.uv, 0.0);
    if (worldPositionSample.w < 0.5) {
        discard;
    }

    float3 worldPosition = worldPositionSample.xyz;
    float4 normalRoughness = normalTexture.SampleLevel(renderSampler, input.uv, 0.0);
    float3 normal = normalize(normalRoughness.xyz);
    float roughness = saturate(normalRoughness.w);
    float3 albedo = albedoTexture.SampleLevel(renderSampler, input.uv, 0.0).rgb;
    float ao = ambientOcclusionTexture.SampleLevel(renderSampler, input.uv, 0.0);

    float viewDepth = max(dot(worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz), 0.0);
    uint cascadeIndex = viewDepth < frame.cascadeSplits.x ? 0
        : viewDepth < frame.cascadeSplits.y ? 1
        : viewDepth < frame.cascadeSplits.z ? 2 : 3;
    float visibility = calculateShadow(worldPosition, normal, cascadeIndex);

    float3 toLight = normalize(-frame.lightDirection.xyz);
    float3 toView = normalize(frame.cameraPosition.xyz - worldPosition);
    float3 halfVector = normalize(toLight + toView);
    float diffuse = max(dot(normal, toLight), 0.0);
    float specularPower = lerp(96.0, 8.0, roughness);
    float specular = pow(max(dot(normal, halfVector), 0.0), specularPower) * (1.0 - roughness) * 0.35;
    float3 sunColor = float3(1.0, 0.94, 0.82) * 3.2;
    float3 ambient = albedo * (0.035 + 0.12 * ao);
    float3 direct = (albedo * diffuse + specular.xxx) * sunColor * visibility;
    return float4(ambient + direct, 1.0);
}

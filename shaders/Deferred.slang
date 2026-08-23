import PostProcessUniforms;
import GpuScene;

[[vk::binding(0, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(1, 0)]] Texture2D<float4> normalTexture;
[[vk::binding(2, 0)]] Texture2D<float4> albedoTexture;
[[vk::binding(4, 0)]] Texture2D<float4> globalIlluminationTexture;
[[vk::binding(8, 0)]] Texture2D<float> shadowTexture0;
[[vk::binding(9, 0)]] Texture2D<float> shadowTexture1;
[[vk::binding(10, 0)]] Texture2D<float> shadowTexture2;
[[vk::binding(11, 0)]] Texture2D<float> shadowTexture3;
[[vk::binding(12, 0)]] SamplerState renderSampler;
[[vk::binding(13, 0)]] ConstantBuffer<PostProcessUniforms> frame;
[[vk::binding(0, 1)]] Texture2D<uint> materialIdTexture;
[[vk::binding(1, 1)]] StructuredBuffer<GpuMaterialData> materials;

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
    float2 shadowUv = float2(shadowNdc.x * 0.5 + 0.5, 0.5 - shadowNdc.y * 0.5);
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

static const float pi = 3.14159265359;

float distributionGgx(float3 normal, float3 halfVector, float roughness) {
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float normalDotHalf = max(dot(normal, halfVector), 0.0);
    float normalDotHalfSquared = normalDotHalf * normalDotHalf;
    float denominator = normalDotHalfSquared * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(pi * denominator * denominator, 1e-5);
}

float geometrySchlickGgx(float normalDotDirection, float roughness) {
    float adjustedRoughness = roughness + 1.0;
    float k = adjustedRoughness * adjustedRoughness / 8.0;
    return normalDotDirection / max(normalDotDirection * (1.0 - k) + k, 1e-5);
}

float geometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness) {
    float normalDotView = max(dot(normal, viewDirection), 0.0);
    float normalDotLight = max(dot(normal, lightDirection), 0.0);
    return geometrySchlickGgx(normalDotView, roughness) * geometrySchlickGgx(normalDotLight, roughness);
}

float3 fresnelSchlick(float cosineTheta, float3 reflectanceAtNormal) {
    return reflectanceAtNormal + (1.0 - reflectanceAtNormal) * pow(saturate(1.0 - cosineTheta), 5.0);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 worldPositionSample = positionTexture.SampleLevel(renderSampler, input.uv, 0.0);
    if (worldPositionSample.w <= 0.0) {
        discard;
    }
    uint materialId = materialIdTexture.Load(int3(int2(input.position.xy), 0));
    if (materialId == 0xffffffffu) {
        discard;
    }
    GpuMaterialData material = materials[materialId];

    float3 worldPosition = worldPositionSample.xyz;
    float4 normalRoughness = normalTexture.SampleLevel(renderSampler, input.uv, 0.0);
    float3 normal = normalize(normalRoughness.xyz);
    float roughness = saturate(normalRoughness.w);
    float4 albedoMetallic = albedoTexture.SampleLevel(renderSampler, input.uv, 0.0);
    float3 albedo = albedoMetallic.rgb;
    float metallic = saturate(albedoMetallic.a);
    float4 globalIllumination = globalIlluminationTexture.SampleLevel(renderSampler, input.uv, 0.0);

    float viewDepth = max(dot(worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz), 0.0);
    uint cascadeIndex = viewDepth < frame.cascadeSplits.x ? 0
        : viewDepth < frame.cascadeSplits.y ? 1
        : viewDepth < frame.cascadeSplits.z ? 2 : 3;
    float visibility = calculateShadow(worldPosition, normal, cascadeIndex);

    float3 toLight = normalize(-frame.lightDirection.xyz);
    float3 toView = normalize(frame.cameraPosition.xyz - worldPosition);
    float3 lightViewSum = toLight + toView;
    float3 halfVector = dot(lightViewSum, lightViewSum) > 1e-8 ? normalize(lightViewSum) : normal;
    float normalDotLight = max(dot(normal, toLight), 0.0);
    float normalDotView = max(dot(normal, toView), 0.0);
    float3 sunColor = float3(1.0, 0.94, 0.82) * 3.2;
    float3 directBrdf;
    float3 legacyAmbient;
    if (material.metadata.x == 1u) {
        // G-buffer 中的逐像素等效粗糙度会同时保留 roughness texture 对高光宽度的影响。
        float exponent = clamp(2.0 / max(roughness * roughness, 1e-5) - 2.0, 1.0, 8192.0);
        float3 specularColor = saturate(material.specularColorShininess.rgb);
        float blinnSpecular = pow(max(dot(normal, halfVector), 0.0), exponent);
        directBrdf = albedo / pi + specularColor * blinnSpecular;
        legacyAmbient = albedo * 0.12 + specularColor * 0.04;
    } else {
        float3 baseReflectance = lerp(0.04.xxx, albedo, metallic);
        float3 fresnel = fresnelSchlick(max(dot(halfVector, toView), 0.0), baseReflectance);
        float distribution = distributionGgx(normal, halfVector, roughness);
        float geometry = geometrySmith(normal, toView, toLight, roughness);
        float3 specular = distribution * geometry * fresnel / max(4.0 * normalDotView * normalDotLight, 1e-4);
        float3 diffuseWeight = (1.0 - fresnel) * (1.0 - metallic);
        directBrdf = diffuseWeight * albedo / pi + specular;
        legacyAmbient = albedo * (1.0 - metallic) * 0.12 + baseReflectance * 0.04;
    }
    float3 ambient = legacyAmbient * globalIllumination.a + globalIllumination.rgb;
    float3 direct = directBrdf * sunColor * normalDotLight * visibility * saturate(frame.lightDirection.w);
    return float4(ambient + direct, 1.0);
}

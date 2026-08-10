#include "GpuScene.slang"
#include "atmosphere/AtmosphereBindings.slang"

struct RayTracedDiConstants {
    float4x4 inverseViewProjection;
    float4x4 previousViewProjection;
    float4 cameraPosition;
    float4 cameraForward;
    float4 toSunWorld;
    float4 sunRadiance;
    float4 renderSize;
    float4 traceParameters;
};

struct PrimaryPayload {
    float3 radiance;
    float hitDistance;
    float3 normal;
    uint materialId;
    uint rayKind;
    uint hit;
    uint visibility;
    uint padding;
};

static const uint kPrimaryRay = 0;
static const uint kShadowRay = 1;
static const uint kInvalidMaterial = 0xffffffffu;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(1, 0)]] StructuredBuffer<GpuPackedVertex> vertexBuffers[];
[[vk::binding(2, 0)]] StructuredBuffer<uint> indexBuffers[];
[[vk::binding(3, 0)]] StructuredBuffer<GpuInstanceData> instances;
[[vk::binding(4, 0)]] StructuredBuffer<GpuMaterialData> materials;
[[vk::binding(5, 0)]] RWTexture2D<float4> worldPositionHitT;
[[vk::binding(6, 0)]] RWTexture2D<float4> normalRoughness;
[[vk::binding(7, 0)]] RWTexture2D<float4> albedoMetallic;
[[vk::binding(8, 0)]] RWTexture2D<uint> materialIdOutput;
[[vk::binding(9, 0)]] RWTexture2D<float> viewZ;
[[vk::binding(10, 0)]] RWTexture2D<float2> motion;
[[vk::binding(11, 0)]] RWTexture2D<float4> directRadiance;
[[vk::binding(12, 0)]] RWTexture2D<uint> visibilityMask;
[[vk::binding(13, 0)]] ConstantBuffer<RayTracedDiConstants> frame;

float2 clipToUv(float4 clipPosition) {
    float2 ndc = clipPosition.xy / max(abs(clipPosition.w), 1e-6);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float3 safeNormalize(float3 value, float3 fallback) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 1e-10 ? value * rsqrt(lengthSquared) : fallback;
}

float3 reconstructDirection(uint2 pixel, uint2 extent) {
    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 farPoint = mul(frame.inverseViewProjection, float4(ndc, 1.0, 1.0));
    farPoint.xyz /= max(abs(farPoint.w), 1e-6);
    return safeNormalize(farPoint.xyz - frame.cameraPosition.xyz, float3(0.0, 0.0, -1.0));
}

float3 evaluateDirect(GpuMaterialData material, float3 normal, float3 toView, float3 toLight, float visibility) {
    float normalDotLight = saturate(dot(normal, toLight));
    if (normalDotLight <= 0.0 || visibility <= 0.0 || frame.traceParameters.z < 0.5) {
        return float3(0.0);
    }
    float3 halfVector = safeNormalize(toView + toLight, normal);
    if (material.metadata.x == 1u) {
        float roughness = clamp(material.surfaceParameters.x, 0.04, 1.0);
        float exponent = clamp(2.0 / max(roughness * roughness, 1e-5) - 2.0, 1.0, 8192.0);
        float specular = pow(saturate(dot(normal, halfVector)), exponent);
        return (material.baseColorMetallic.rgb / 3.14159265359 +
                saturate(material.specularColorShininess.rgb) * specular) *
               frame.sunRadiance.rgb * normalDotLight * visibility;
    }

    float metallic = saturate(material.baseColorMetallic.a);
    float roughness = clamp(material.surfaceParameters.x, 0.04, 1.0);
    float3 f0 = lerp(float3(0.04), material.baseColorMetallic.rgb, metallic);
    float fresnel = pow(1.0 - saturate(dot(halfVector, toView)), 5.0);
    float3 F = f0 + (1.0 - f0) * fresnel;
    float3 diffuse = (1.0 - F) * (1.0 - metallic) * material.baseColorMetallic.rgb / 3.14159265359;
    float specularExponent = clamp(2.0 / max(roughness * roughness, 1e-5) - 2.0, 1.0, 8192.0);
    float specular = pow(saturate(dot(normal, halfVector)), specularExponent);
    return (diffuse + F * specular) * frame.sunRadiance.rgb * normalDotLight * visibility;
}

void writeMiss(uint2 pixel, float3 environmentRadiance) {
    worldPositionHitT[pixel] = float4(0.0, 0.0, 0.0, -1.0);
    normalRoughness[pixel] = float4(0.0, 0.0, 0.0, 1.0);
    albedoMetallic[pixel] = float4(0.0);
    materialIdOutput[pixel] = kInvalidMaterial;
    viewZ[pixel] = 0.0;
    motion[pixel] = float2(0.0);
    directRadiance[pixel] = float4(environmentRadiance, 1.0);
    visibilityMask[pixel] = 0u;
}

[shader("raygeneration")]
void rayGenerationMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 extent = DispatchRaysDimensions().xy;
    if (any(pixel >= extent)) {
        return;
    }

    RayDesc ray;
    ray.Origin = frame.cameraPosition.xyz;
    ray.Direction = reconstructDirection(pixel, extent);
    ray.TMin = frame.traceParameters.x;
    ray.TMax = frame.traceParameters.y;

    PrimaryPayload payload;
    payload.radiance = float3(0.0);
    payload.hitDistance = ray.TMax;
    payload.normal = float3(0.0);
    payload.materialId = kInvalidMaterial;
    payload.rayKind = kPrimaryRay;
    payload.hit = 0u;
    payload.visibility = 0u;
    payload.padding = 0u;
    TraceRay(sceneTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xff, 0, 1, 0, ray, payload);
}

[shader("miss")]
void primaryMissMain(inout PrimaryPayload payload) {
    if (payload.rayKind == kShadowRay) {
        payload.visibility = 1u;
        return;
    }
    uint2 pixel = DispatchRaysIndex().xy;
    writeMiss(pixel, atmosphereSampleEnvironmentRadiance(WorldRayDirection()) * frame.sunRadiance.w);
}

[shader("miss")]
void shadowMissMain(inout PrimaryPayload payload) {
    payload.visibility = 1u;
}

[shader("closesthit")]
void primaryClosestHitMain(inout PrimaryPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    if (payload.rayKind == kShadowRay) {
        payload.visibility = 0u;
        return;
    }

    uint2 pixel = DispatchRaysIndex().xy;
    uint2 extent = DispatchRaysDimensions().xy;
    GpuInstanceData instance = instances[InstanceID()];
    uint geometryIndex = instance.metadata.w;
    StructuredBuffer<GpuPackedVertex> vertices = vertexBuffers[NonUniformResourceIndex(geometryIndex)];
    StructuredBuffer<uint> indices = indexBuffers[NonUniformResourceIndex(geometryIndex)];
    uint triangleIndex = PrimitiveIndex() * 3 + instance.metadata.y;
    uint3 vertexIndices = uint3(indices[triangleIndex], indices[triangleIndex + 1], indices[triangleIndex + 2]);
    float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                 attributes.barycentrics.x, attributes.barycentrics.y);
    float3 objectNormal = normalize(vertices[vertexIndices.x + instance.metadata.z].normal.xyz * barycentrics.x +
                                    vertices[vertexIndices.y + instance.metadata.z].normal.xyz * barycentrics.y +
                                    vertices[vertexIndices.z + instance.metadata.z].normal.xyz * barycentrics.z);
    float3 normal = safeNormalize(mul((float3x3)instance.normalMatrix, objectNormal), float3(0.0, 1.0, 0.0));
    float3 hitPosition = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 toLight = safeNormalize(frame.toSunWorld.xyz, float3(0.0, 1.0, 0.0));
    float3 toView = safeNormalize(frame.cameraPosition.xyz - hitPosition, normal);

    RayDesc shadowRay;
    shadowRay.Origin = hitPosition + normal * frame.traceParameters.x;
    shadowRay.Direction = toLight;
    shadowRay.TMin = frame.traceParameters.x;
    shadowRay.TMax = frame.traceParameters.y;
    PrimaryPayload shadowPayload;
    shadowPayload.radiance = float3(0.0);
    shadowPayload.hitDistance = shadowRay.TMax;
    shadowPayload.normal = float3(0.0);
    shadowPayload.materialId = kInvalidMaterial;
    shadowPayload.rayKind = kShadowRay;
    shadowPayload.hit = 0u;
    shadowPayload.visibility = 0u;
    shadowPayload.padding = 0u;
    TraceRay(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
             0xff, 0, 1, 1, shadowRay, shadowPayload);

    GpuMaterialData material = materials[instance.metadata.x];
    float3 direct = evaluateDirect(material, normal, toView, toLight, float(shadowPayload.visibility));
    // 光线由当前抖动投影生成，dispatch 像素中心已经是当前采样位置，不能再次叠加 jitter。
    // previousViewProjection 保留上一帧抖动，因此两者相减会自然得到 TAA 所需的 previous-current motion。
    float2 currentUv = (float2(pixel) + 0.5) / float2(extent);
    float4 previousClip = mul(frame.previousViewProjection, float4(hitPosition, 1.0));
    float2 previousUv = clipToUv(previousClip);
    float depth = max(dot(hitPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz), 0.0);

    worldPositionHitT[pixel] = float4(hitPosition, RayTCurrent());
    normalRoughness[pixel] = float4(normal, clamp(material.surfaceParameters.x, 0.04, 1.0));
    albedoMetallic[pixel] = material.baseColorMetallic;
    materialIdOutput[pixel] = instance.metadata.x;
    viewZ[pixel] = depth;
    motion[pixel] = previousUv - currentUv;
    directRadiance[pixel] = float4(direct, 1.0);
    visibilityMask[pixel] = 1u;
}

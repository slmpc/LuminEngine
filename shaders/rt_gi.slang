#include "GpuScene.slang"
#include "NRD.hlsli"
#include "atmosphere/AtmosphereBindings.slang"

#ifndef LUMIN_ENABLE_SHARC
#define LUMIN_ENABLE_SHARC 0
#endif

#if LUMIN_ENABLE_SHARC
#define LUMIN_SHARC_UPDATE 0
#include "SharcRuntime.slang"
#endif

struct RayTracedGiConstants {
    float4 cameraPosition;
    float4 cameraForward;
    float4 toSunWorld;
    float4 sunRadiance;
    float4 renderSize;
    float4 traceParameters;
};

struct GiPayload {
    float3 radiance;
    float hitDistance;
    uint rayKind;
    float visibility;
};

static const uint kRadianceRay = 0;
static const uint kShadowRay = 1;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(1, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(2, 0)]] Texture2D<float4> normalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float4> albedoMetallicTexture;
[[vk::binding(4, 0)]] Texture2D<float2> motionTexture;
[[vk::binding(5, 0)]] StructuredBuffer<GpuPackedVertex> vertexBuffers[];
[[vk::binding(6, 0)]] StructuredBuffer<uint> indexBuffers[];
[[vk::binding(7, 0)]] StructuredBuffer<GpuInstanceData> instances;
[[vk::binding(8, 0)]] StructuredBuffer<GpuMaterialData> materials;
[[vk::binding(9, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> diffuseRadianceHitDistance;
[[vk::binding(10, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> specularRadianceHitDistance;
[[vk::binding(11, 0), vk::image_format("r32f")]] RWTexture2D<float> viewZ;
[[vk::binding(12, 0), vk::image_format("rgb10_a2")]] RWTexture2D<float4> denoiserNormalRoughness;
[[vk::binding(13, 0), vk::image_format("rg16f")]] RWTexture2D<float2> denoiserMotion;
[[vk::binding(14, 0)]] ConstantBuffer<RayTracedGiConstants> frame;

#if LUMIN_ENABLE_SHARC
[[vk::binding(15, 0)]] RWStructuredBuffer<uint64_t> sharcHashEntries;
[[vk::binding(16, 0)]] RWStructuredBuffer<SharcAccumulationData> sharcAccumulation;
[[vk::binding(17, 0)]] RWStructuredBuffer<SharcPackedData> sharcResolved;
[[vk::binding(18, 0)]] RWStructuredBuffer<uint> sharcLocks;
[[vk::binding(19, 0)]] RWStructuredBuffer<uint> sharcStatistics;
[[vk::binding(20, 0)]] ConstantBuffer<SharcGpuConstants> sharcFrame;

static const uint kSharcStatisticQueryHit = 0;

SharcParameters rayTracedGiSharcParameters() {
    return luminMakeSharcParameters(
        sharcFrame, sharcHashEntries, sharcAccumulation, sharcResolved, sharcLocks);
}
#endif

uint hashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    return value ^ (value >> 16);
}

float random01(uint seed) {
    return float(hashUint(seed) & 0x00ffffff) * (1.0 / 16777216.0);
}

float3 cosineHemisphere(float3 normal, uint seed) {
    float2 random = float2(random01(seed), random01(seed ^ 0x9e3779b9));
    float radius = sqrt(random.x);
    float angle = 6.28318530718 * random.y;
    float3 localDirection = float3(radius * cos(angle), radius * sin(angle), sqrt(1.0 - random.x));
    float3 tangent = normalize(abs(normal.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), normal)
                                                      : cross(float3(0.0, 1.0, 0.0), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * localDirection.x + bitangent * localDirection.y + normal * localDirection.z);
}

GiPayload traceRadiance(float3 origin, float3 direction) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = frame.traceParameters.x;
    ray.TMax = frame.traceParameters.y;

    GiPayload payload;
    payload.radiance = float3(0.0);
    payload.hitDistance = ray.TMax;
    payload.rayKind = kRadianceRay;
    payload.visibility = 0.0;
    TraceRay(sceneTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xff, 0, 1, 0, ray, payload);
    return payload;
}

[shader("raygeneration")]
void rayGenerationMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 extent = DispatchRaysDimensions().xy;
    if (any(pixel >= extent)) {
        return;
    }

    float4 positionSample = positionTexture.Load(int3(pixel, 0));
    float4 normalRoughness = normalRoughnessTexture.Load(int3(pixel, 0));
    float2 motion = motionTexture.Load(int3(pixel, 0));
    // RT surface motion 包含投影 jitter；NRD 要求 previous-current 且不含 jitter。
    // renderSize.zw 是 currentEffectiveJitter - previousEffectiveJitter，正好抵消 surface motion 中的抖动差。
    denoiserMotion[pixel] = motion + frame.renderSize.zw;

    // RT surface 将命中距离写入 position.w；miss 为负值，不能用小于 0.5 判断有效性。
    if (positionSample.w <= 0.0) {
        diffuseRadianceHitDistance[pixel] = float4(0.0);
        specularRadianceHitDistance[pixel] = float4(0.0);
        viewZ[pixel] = frame.traceParameters.w;
        denoiserNormalRoughness[pixel] = float4(0.0);
        return;
    }

    float3 worldPosition = positionSample.xyz;
    float3 normal = normalize(normalRoughness.xyz);
    float roughness = saturate(normalRoughness.w);
    uint linearIndex = pixel.y * extent.x + pixel.x;
    uint seed = linearIndex ^ (uint(frame.traceParameters.z) * 0x68bc21eb);
    float3 diffuseDirection = cosineHemisphere(normal, seed);
    float3 viewIncident = normalize(worldPosition - frame.cameraPosition.xyz);
    float3 reflectedDirection = reflect(viewIncident, normal);
    float3 specularDirection = normalize(lerp(reflectedDirection, diffuseDirection, roughness * roughness));
    float3 origin = worldPosition + normal * frame.traceParameters.x;

    GiPayload diffusePayload = traceRadiance(origin, diffuseDirection);
    GiPayload specularPayload = traceRadiance(origin, specularDirection);
    float primaryViewZ = max(dot(worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz), 0.0);
    float3 hitDistanceParameters = float3(3.0, 0.1, 20.0);
    float diffuseNormHitDistance = REBLUR_FrontEnd_GetNormHitDist(
        diffusePayload.hitDistance, primaryViewZ, hitDistanceParameters, roughness);
    float specularNormHitDistance = REBLUR_FrontEnd_GetNormHitDist(
        specularPayload.hitDistance, primaryViewZ, hitDistanceParameters, roughness);

    // NRD 输入保持材质解调：primary albedo/F0 由 denoise 后的 composite 再乘回。
    diffuseRadianceHitDistance[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        diffusePayload.radiance, diffuseNormHitDistance, true);
    specularRadianceHitDistance[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        specularPayload.radiance, specularNormHitDistance, true);
    denoiserNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0.0);
    viewZ[pixel] = primaryViewZ;
}

[shader("miss")]
void radianceMissMain(inout GiPayload payload) {
    float3 direction = normalize(WorldRayDirection());
    payload.radiance = atmosphereSampleEnvironmentRadiance(direction) * frame.sunRadiance.w;
    payload.hitDistance = frame.traceParameters.y;
}

[shader("miss")]
void shadowMissMain(inout GiPayload payload) {
    payload.visibility = 1.0;
}

[shader("closesthit")]
void closestHitMain(inout GiPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    if (payload.rayKind == kShadowRay) {
        payload.visibility = 0.0;
        return;
    }

    uint instanceIndex = InstanceID();
    GpuInstanceData instance = instances[instanceIndex];
    uint geometryIndex = instance.metadata.w;
    StructuredBuffer<GpuPackedVertex> vertices = vertexBuffers[NonUniformResourceIndex(geometryIndex)];
    StructuredBuffer<uint> indices = indexBuffers[NonUniformResourceIndex(geometryIndex)];
    uint firstIndex = instance.metadata.y;
    uint vertexOffset = instance.metadata.z;
    uint triangleIndex = PrimitiveIndex() * 3 + firstIndex;
    uint3 vertexIndices = uint3(indices[triangleIndex], indices[triangleIndex + 1], indices[triangleIndex + 2]);
    float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                attributes.barycentrics.x, attributes.barycentrics.y);
    float3 objectNormal = normalize(vertices[vertexIndices.x + vertexOffset].normal * barycentrics.x +
                                    vertices[vertexIndices.y + vertexOffset].normal * barycentrics.y +
                                    vertices[vertexIndices.z + vertexOffset].normal * barycentrics.z);
    float3 worldNormal = normalize(mul((float3x3)instance.normalMatrix, objectNormal));
    float3 hitPosition = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 toSun = normalize(frame.toSunWorld.xyz);

    RayDesc shadowRay;
    shadowRay.Origin = hitPosition + worldNormal * frame.traceParameters.x;
    shadowRay.Direction = toSun;
    shadowRay.TMin = frame.traceParameters.x;
    shadowRay.TMax = frame.traceParameters.y;
    GiPayload shadowPayload;
    shadowPayload.radiance = float3(0.0);
    shadowPayload.hitDistance = shadowRay.TMax;
    shadowPayload.rayKind = kShadowRay;
    shadowPayload.visibility = 0.0;
    TraceRay(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
             0xff, 0, 1, 1, shadowRay, shadowPayload);

    GpuMaterialData material = materials[instance.metadata.x];
    float noL = saturate(dot(worldNormal, toSun));
    float3 direct = material.baseColorMetallic.rgb * frame.sunRadiance.rgb * (noL * shadowPayload.visibility);
    float3 fallbackRadiance = direct + material.baseColorMetallic.rgb * 0.015;
#if LUMIN_ENABLE_SHARC
    SharcHitData sharcHit;
    sharcHit.positionWorld = hitPosition;
    sharcHit.normalWorld = worldNormal;
    float3 cachedRadiance;
    if (SharcGetCachedRadiance(rayTracedGiSharcParameters(), sharcHit, cachedRadiance, false)) {
        payload.radiance = cachedRadiance;
        InterlockedAdd(sharcStatistics[kSharcStatisticQueryHit], 1);
    } else {
        payload.radiance = fallbackRadiance;
    }
#else
    payload.radiance = fallbackRadiance;
#endif
    payload.hitDistance = RayTCurrent();
}

#include "GpuScene.slang"
#include "PhysicalLighting.slang"
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
    float4 sunIrradiance;
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
static const uint kInvalidMaterial = 0xffffffffu;

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

[[vk::binding(21, 0)]] Texture2D<float4> baseColorTextures[];
[[vk::binding(22, 0)]] Texture2D<float4> normalRoughnessTextures[];
[[vk::binding(23, 0)]] SamplerState materialSampler;
[[vk::binding(24, 0)]] Texture2D<uint> materialIdTexture;

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
    TraceRay(sceneTlas, 0, 0xff, 0, 1, 0, ray, payload);
    return payload;
}

GiPayload emptyRadiancePayload() {
    GiPayload payload;
    payload.radiance = float3(0.0);
    payload.hitDistance = frame.traceParameters.y;
    payload.rayKind = kRadianceRay;
    payload.visibility = 0.0;
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
    uint materialId = materialIdTexture.Load(int3(pixel, 0));
    if (positionSample.w <= 0.0 || materialId == kInvalidMaterial) {
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
    GpuMaterialData primaryMaterial = materials[materialId];
    primaryMaterial.baseColorMetallic = albedoMetallicTexture.Load(int3(pixel, 0));
    primaryMaterial.surfaceParameters.x = roughness;
    LuminCookTorranceMaterial cookTorranceMaterial = luminMakeCookTorranceMaterial(primaryMaterial);
    float3 toView = luminPhysicalSafeNormalize(frame.cameraPosition.xyz - worldPosition, normal);
    LuminBrdfSample diffuseSample = luminSampleCookTorranceDiffuse(
        cookTorranceMaterial, normal, toView,
        float2(random01(seed), random01(seed ^ 0x9e3779b9)));
    LuminBrdfSample specularSample = luminSampleCookTorranceSpecular(
        cookTorranceMaterial, normal, toView,
        float2(random01(seed ^ 0x243f6a88), random01(seed ^ 0xb7e15162)));
    float3 origin = worldPosition + normal * frame.traceParameters.x;

    GiPayload diffusePayload = diffuseSample.valid != 0u
        ? traceRadiance(origin, diffuseSample.direction) : emptyRadiancePayload();
    GiPayload specularPayload = specularSample.valid != 0u
        ? traceRadiance(origin, specularSample.direction) : emptyRadiancePayload();
    float primaryViewZ = max(dot(worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz), 0.0);
    float3 hitDistanceParameters = float3(3.0, 0.1, 20.0);
    float diffuseNormHitDistance = REBLUR_FrontEnd_GetNormHitDist(
        diffusePayload.hitDistance, primaryViewZ, hitDistanceParameters, roughness);
    float specularNormHitDistance = REBLUR_FrontEnd_GetNormHitDist(
        specularPayload.hitDistance, primaryViewZ, hitDistanceParameters, roughness);

    // 每个 signal 已包含 Cook-Torrance BRDF * cos(theta) / PDF；去噪和 composite 不得再次乘材质项。
    diffuseRadianceHitDistance[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        diffusePayload.radiance * diffuseSample.throughput, diffuseNormHitDistance, true);
    specularRadianceHitDistance[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        specularPayload.radiance * specularSample.throughput, specularNormHitDistance, true);
    denoiserNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0.0);
    viewZ[pixel] = primaryViewZ;
}

[shader("miss")]
void radianceMissMain(inout GiPayload payload) {
    float3 direction = normalize(WorldRayDirection());
    payload.radiance = atmosphereSampleEnvironmentRadiance(direction) * frame.sunIrradiance.w;
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
    GpuPackedVertex vertex0 = vertices[vertexIndices.x + vertexOffset];
    GpuPackedVertex vertex1 = vertices[vertexIndices.y + vertexOffset];
    GpuPackedVertex vertex2 = vertices[vertexIndices.z + vertexOffset];
    float3 objectNormal = normalize(vertex0.normal * barycentrics.x + vertex1.normal * barycentrics.y +
                                    vertex2.normal * barycentrics.z);
    float3 worldNormal = luminOrientShadingNormal(mul((float3x3)instance.normalMatrix, objectNormal),
                                                  WorldRayDirection());
    float2 texCoord = luminVertexTexCoord(vertex0) * barycentrics.x +
                      luminVertexTexCoord(vertex1) * barycentrics.y +
                      luminVertexTexCoord(vertex2) * barycentrics.z;
    GpuMaterialData material = materials[instance.metadata.x];
    if (material.metadata.z != 0u) {
        uint textureIndex = material.metadata.y;
        float2 materialUv = texCoord * material.surfaceParameters.y;
        material.baseColorMetallic.rgb *=
            baseColorTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(materialSampler, materialUv, 0.0).rgb;
        material.surfaceParameters.x = clamp(
            normalRoughnessTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(materialSampler, materialUv, 0.0).a *
                material.surfaceParameters.x,
            0.04, 1.0);
    }
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
    TraceRay(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, 0, 1, 1, shadowRay, shadowPayload);

    float noL = saturate(dot(worldNormal, toSun));
    float3 toView = luminPhysicalSafeNormalize(-WorldRayDirection(), worldNormal);
    LuminCookTorranceLobes lobes = luminEvaluateCookTorrance(
        luminMakeCookTorranceMaterial(material), worldNormal, toView, toSun);
    float3 direct = (lobes.diffuse + lobes.specular) * frame.sunIrradiance.rgb *
                    (noL * shadowPayload.visibility);
#if LUMIN_ENABLE_SHARC
    SharcHitData sharcHit;
    sharcHit.positionWorld = hitPosition;
    sharcHit.normalWorld = worldNormal;
    float3 cachedRadiance;
    if (SharcGetCachedRadiance(rayTracedGiSharcParameters(), sharcHit, cachedRadiance, false)) {
        payload.radiance = cachedRadiance;
        InterlockedAdd(sharcStatistics[kSharcStatisticQueryHit], 1);
    } else {
        payload.radiance = direct;
    }
#else
    payload.radiance = direct;
#endif
    payload.hitDistance = RayTCurrent();
}

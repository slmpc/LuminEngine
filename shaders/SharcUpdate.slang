import GpuScene;
import atmosphere.AtmosphereBindings;
#define LUMIN_SHARC_UPDATE 1
#include "SharcRuntime.slang"

struct SharcUpdatePayload {
    SharcState cacheState;
    float3 throughput;
    uint rayKind;
    float visibility;
};

static const uint kSharcRadianceRay = 0;
static const uint kSharcShadowRay = 1;
static const uint kSharcStatisticUpdate = 1;
static const uint kSharcStatisticOverflow = 2;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(1, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(2, 0)]] Texture2D<float4> normalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float4> albedoMetallicTexture;
[[vk::binding(4, 0)]] StructuredBuffer<GpuPackedVertex> vertexBuffers[];
[[vk::binding(5, 0)]] StructuredBuffer<uint> indexBuffers[];
[[vk::binding(6, 0)]] StructuredBuffer<GpuInstanceData> instances;
[[vk::binding(7, 0)]] StructuredBuffer<GpuMaterialData> materials;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint64_t> sharcHashEntries;
[[vk::binding(9, 0)]] RWStructuredBuffer<SharcAccumulationData> sharcAccumulation;
[[vk::binding(10, 0)]] RWStructuredBuffer<SharcPackedData> sharcResolved;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> sharcLocks;
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> sharcStatistics;
[[vk::binding(13, 0)]] ConstantBuffer<SharcGpuConstants> sharcFrame;
[[vk::binding(14, 0)]] Texture2D<float4> baseColorTextures[];
[[vk::binding(15, 0)]] Texture2D<float4> normalRoughnessTextures[];
[[vk::binding(16, 0)]] SamplerState materialSampler;

uint sharcHashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    return value ^ (value >> 16);
}

float sharcRandom01(uint seed) {
    return float(sharcHashUint(seed) & 0x00ffffff) * (1.0 / 16777216.0);
}

float3 sharcCosineHemisphere(float3 normal, uint seed) {
    float2 random = float2(sharcRandom01(seed), sharcRandom01(seed ^ 0x9e3779b9));
    float radius = sqrt(random.x);
    float angle = 6.28318530718 * random.y;
    float3 localDirection = float3(radius * cos(angle), radius * sin(angle), sqrt(1.0 - random.x));
    float3 tangent = normalize(abs(normal.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), normal)
                                                      : cross(float3(0.0, 1.0, 0.0), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * localDirection.x + bitangent * localDirection.y + normal * localDirection.z);
}

SharcParameters sharcParameters() {
    return luminMakeSharcParameters(
        sharcFrame, sharcHashEntries, sharcAccumulation, sharcResolved, sharcLocks);
}

bool sharcRecordHit(inout SharcUpdatePayload payload, SharcHitData hitData, float3 directLighting, float random) {
    SharcParameters parameters = sharcParameters();
    bool continueTracing = SharcUpdateHit(parameters, payload.cacheState, hitData, directLighting, random);
    HashGridIndex cacheIndex = HashMapFindEntry(
        parameters.hashMapData, hitData.positionWorld, hitData.normalWorld, parameters.gridParameters);
    if (cacheIndex == HASH_GRID_INVALID_CACHE_INDEX) {
        InterlockedAdd(sharcStatistics[kSharcStatisticOverflow], 1);
    } else {
        InterlockedAdd(sharcStatistics[kSharcStatisticUpdate], 1);
    }
    return continueTracing;
}

[shader("raygeneration")]
void sharcUpdateRayGenerationMain() {
    uint2 tile = DispatchRaysIndex().xy;
    uint tileSize = sharcFrame.renderParameters.z;
    uint frameIndex = sharcFrame.cacheParameters.w;
    uint tileSeed = sharcHashUint(tile.x ^ (tile.y * 0x68bc21eb) ^ (frameIndex * 0x9e3779b9));
    uint2 pixel = tile * tileSize + uint2(tileSeed % tileSize, (tileSeed / tileSize) % tileSize);
    uint2 renderExtent = sharcFrame.renderParameters.xy;
    if (any(pixel >= renderExtent)) {
        return;
    }

    float4 positionSample = positionTexture.Load(int3(pixel, 0));
    // RT surface 的 position.w 是 hit distance，miss 为负值。
    if (positionSample.w <= 0.0) {
        return;
    }
    float3 worldPosition = positionSample.xyz;
    float3 normal = normalize(normalRoughnessTexture.Load(int3(pixel, 0)).xyz);
    float3 albedo = max(albedoMetallicTexture.Load(int3(pixel, 0)).rgb, float3(0.0));

    SharcUpdatePayload payload;
    SharcInit(payload.cacheState);
    payload.throughput = albedo;
    payload.rayKind = kSharcRadianceRay;
    payload.visibility = 0.0;

    SharcHitData primaryHit;
    primaryHit.positionWorld = worldPosition;
    primaryHit.normalWorld = normal;
    if (!sharcRecordHit(payload, primaryHit, float3(0.0), sharcRandom01(tileSeed))) {
        return;
    }
    SharcSetThroughput(payload.cacheState, payload.throughput);

    RayDesc ray;
    ray.Origin = worldPosition + normal * sharcFrame.traceParameters.x;
    ray.Direction = sharcCosineHemisphere(normal, tileSeed ^ 0x51ed270b);
    ray.TMin = sharcFrame.traceParameters.x;
    ray.TMax = sharcFrame.traceParameters.y;
    TraceRay(sceneTlas, 0, 0xff, 0, 1, 0, ray, payload);
}

[shader("miss")]
void sharcUpdateRadianceMissMain(inout SharcUpdatePayload payload) {
    float3 skyRadiance = atmosphereSampleEnvironmentRadiance(WorldRayDirection()) * sharcFrame.sunRadiance.w;
    SharcUpdateMiss(sharcParameters(), payload.cacheState, skyRadiance);
}

[shader("miss")]
void sharcUpdateShadowMissMain(inout SharcUpdatePayload payload) {
    payload.visibility = 1.0;
}

[shader("closesthit")]
void sharcUpdateClosestHitMain(
    inout SharcUpdatePayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    if (payload.rayKind == kSharcShadowRay) {
        payload.visibility = 0.0;
        return;
    }

    GpuInstanceData instance = instances[InstanceID()];
    uint geometryIndex = instance.metadata.w;
    StructuredBuffer<GpuPackedVertex> vertices = vertexBuffers[NonUniformResourceIndex(geometryIndex)];
    StructuredBuffer<uint> indices = indexBuffers[NonUniformResourceIndex(geometryIndex)];
    uint triangleIndex = PrimitiveIndex() * 3 + instance.metadata.y;
    uint3 vertexIndices = uint3(indices[triangleIndex], indices[triangleIndex + 1], indices[triangleIndex + 2]);
    float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                 attributes.barycentrics.x, attributes.barycentrics.y);
    GpuPackedVertex vertex0 = vertices[vertexIndices.x + instance.metadata.z];
    GpuPackedVertex vertex1 = vertices[vertexIndices.y + instance.metadata.z];
    GpuPackedVertex vertex2 = vertices[vertexIndices.z + instance.metadata.z];
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
    float3 toSun = normalize(sharcFrame.toSunWorldRadianceScale.xyz);

    SharcUpdatePayload shadowPayload;
    SharcInit(shadowPayload.cacheState);
    shadowPayload.throughput = float3(1.0);
    shadowPayload.rayKind = kSharcShadowRay;
    shadowPayload.visibility = 0.0;
    RayDesc shadowRay;
    shadowRay.Origin = hitPosition + worldNormal * sharcFrame.traceParameters.x;
    shadowRay.Direction = toSun;
    shadowRay.TMin = sharcFrame.traceParameters.x;
    shadowRay.TMax = sharcFrame.traceParameters.y;
    TraceRay(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, 0, 1, 1, shadowRay, shadowPayload);

    float noL = saturate(dot(worldNormal, toSun));
    float3 directLighting = material.baseColorMetallic.rgb * sharcFrame.sunRadiance.rgb *
                            (noL * shadowPayload.visibility);
    SharcHitData hitData;
    hitData.positionWorld = hitPosition;
    hitData.normalWorld = worldNormal;
    sharcRecordHit(payload, hitData, directLighting, sharcRandom01(PrimitiveIndex() ^ InstanceID()));
}

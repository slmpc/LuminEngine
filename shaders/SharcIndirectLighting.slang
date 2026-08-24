import GpuScene;
import PhysicalLighting;
import atmosphere.AtmosphereBindings;
#include "NRD.hlsli"
#define LUMIN_SHARC_UPDATE 0
#include "SharcRuntime.slang"

struct SharcIndirectLightingConstants {
    float4 cameraPosition;
    float4 cameraForward;
    float4 renderParameters;
    float4 traceParameters;
    uint4 samplingParameters;
};

struct IndirectPayload {
    float3 radiance;
    float hitDistance;
    uint rayKind;
    float visibility;
    uint specularPath;
    float pathRoughness;
};

static const uint kIndirectRadianceRay = 0u;
static const uint kIndirectShadowRay = 1u;
static const uint kInvalidMaterial = 0xffffffffu;
static const uint kSharcStatisticQueryHit = 0u;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure sceneTlas;
[[vk::binding(1, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(2, 0)]] Texture2D<float4> normalRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float4> albedoMetallicTexture;
[[vk::binding(4, 0)]] Texture2D<float2> motionTexture;
[[vk::binding(5, 0)]] Texture2D<uint> materialIdTexture;
[[vk::binding(6, 0)]] StructuredBuffer<GpuPackedVertex> vertexBuffers[];
[[vk::binding(7, 0)]] StructuredBuffer<uint> indexBuffers[];
[[vk::binding(8, 0)]] StructuredBuffer<GpuInstanceData> instances;
[[vk::binding(9, 0)]] StructuredBuffer<GpuMaterialData> materials;
[[vk::binding(10, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> diffuseRadianceHitDistance;
[[vk::binding(11, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> specularRadianceHitDistance;
[[vk::binding(12, 0), vk::image_format("r32f")]] RWTexture2D<float> viewZ;
[[vk::binding(13, 0), vk::image_format("rgb10_a2")]] RWTexture2D<float4> denoiserNormalRoughness;
[[vk::binding(14, 0), vk::image_format("rg16f")]] RWTexture2D<float2> denoiserMotion;
[[vk::binding(15, 0)]] ConstantBuffer<SharcIndirectLightingConstants> frame;
[[vk::binding(16, 0)]] RWStructuredBuffer<uint64_t> sharcHashEntries;
[[vk::binding(17, 0)]] RWStructuredBuffer<SharcAccumulationData> sharcAccumulation;
[[vk::binding(18, 0)]] RWStructuredBuffer<SharcPackedData> sharcResolved;
[[vk::binding(19, 0)]] RWStructuredBuffer<uint> sharcLocks;
[[vk::binding(20, 0)]] RWStructuredBuffer<uint> sharcStatistics;
[[vk::binding(21, 0)]] ConstantBuffer<SharcGpuConstants> sharcFrame;
[[vk::binding(22, 0)]] Texture2D<float4> baseColorTextures[];
[[vk::binding(23, 0)]] Texture2D<float4> normalRoughnessTextures[];
[[vk::binding(24, 0)]] SamplerState materialSampler;
[[vk::binding(25, 0)]] StructuredBuffer<GpuLightData> lights;

SharcParameters indirectSharcParameters() {
    return luminMakeSharcParameters(
        sharcFrame, sharcHashEntries, sharcAccumulation, sharcResolved, sharcLocks);
}

float luminance(float3 value) {
    return dot(max(value, float3(0.0)), float3(0.2126, 0.7152, 0.0722));
}

float diffuseSelectionProbability(LuminCookTorranceMaterial material) {
    float diffuseEnergy = luminance(material.diffuseColor);
    float specularEnergy = luminance(material.reflectanceAtNormal);
    if (diffuseEnergy <= 1.0e-6) {
        return 0.0;
    }
    if (specularEnergy <= 1.0e-6) {
        return 1.0;
    }
    return clamp(diffuseEnergy / (diffuseEnergy + specularEnergy), 0.05, 0.95);
}

IndirectPayload traceIndirect(float3 origin, LuminBrdfSample sample, bool specularPath, float pathRoughness) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = sample.direction;
    ray.TMin = frame.traceParameters.x;
    ray.TMax = frame.traceParameters.y;
    IndirectPayload payload;
    payload.radiance = float3(0.0);
    payload.hitDistance = ray.TMax;
    payload.rayKind = kIndirectRadianceRay;
    payload.visibility = 0.0;
    payload.specularPath = specularPath ? 1u : 0u;
    payload.pathRoughness = pathRoughness;
    TraceRay(sceneTlas, 0, 0xff, 0, 1, 0, ray, payload);
    return payload;
}

bool cacheFootprintIsUsable(IndirectPayload payload, float3 hitPosition) {
    SharcParameters parameters = indirectSharcParameters();
    uint gridLevel = uint(HashGridCalculatePositionLog(hitPosition, parameters.gridParameters).w);
    float voxelSize = HashGridGetVoxelSize(gridLevel, parameters.gridParameters);
    if (payload.hitDistance <= voxelSize) {
        return false;
    }
    if (payload.specularPath == 0u) {
        return true;
    }
    float alpha = payload.pathRoughness * payload.pathRoughness;
    float alphaSquared = min(alpha * alpha, 1.0 - 1.0e-6);
    float footprint = 2.0 * payload.hitDistance * sqrt(0.5 * alphaSquared / max(1.0 - alphaSquared, 1.0e-6));
    return footprint >= voxelSize;
}

[shader("raygeneration")]
void sharcIndirectLightingRayGenerationMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 extent = DispatchRaysDimensions().xy;
    if (any(pixel >= extent)) {
        return;
    }
    denoiserMotion[pixel] = motionTexture.Load(int3(pixel, 0)) + frame.renderParameters.zw;
    float4 positionSample = positionTexture.Load(int3(pixel, 0));
    uint materialId = materialIdTexture.Load(int3(pixel, 0));
    if (positionSample.w <= 0.0 || materialId == kInvalidMaterial) {
        diffuseRadianceHitDistance[pixel] = float4(0.0);
        specularRadianceHitDistance[pixel] = float4(0.0);
        denoiserNormalRoughness[pixel] = float4(0.0);
        viewZ[pixel] = frame.traceParameters.w;
        return;
    }

    float4 normalRoughness = normalRoughnessTexture.Load(int3(pixel, 0));
    float3 normal = normalize(normalRoughness.xyz);
    GpuMaterialData material = materials[materialId];
    material.baseColorMetallic = albedoMetallicTexture.Load(int3(pixel, 0));
    material.surfaceParameters.x = saturate(normalRoughness.w);
    LuminCookTorranceMaterial physicalMaterial = luminMakeCookTorranceMaterial(material);
    float3 toView = luminPhysicalSafeNormalize(frame.cameraPosition.xyz - positionSample.xyz, normal);
    float3 diffuseMaterialFactor;
    float3 specularMaterialFactor;
    NRD_MaterialFactors(normal, toView, physicalMaterial.diffuseColor, physicalMaterial.reflectanceAtNormal,
                        physicalMaterial.roughness, diffuseMaterialFactor, specularMaterialFactor);
    float primaryViewZ = max(dot(positionSample.xyz - frame.cameraPosition.xyz, frame.cameraForward.xyz), 0.0);
    denoiserNormalRoughness[pixel] =
        NRD_FrontEnd_PackNormalAndRoughness(normal, physicalMaterial.roughness, 0.0);
    viewZ[pixel] = primaryViewZ;

    uint seed = luminHashCombine(luminHashCombine(pixel.x, pixel.y), uint(frame.traceParameters.z));
    seed = luminHashCombine(seed, materialId);
    float diffuseProbability = diffuseSelectionProbability(physicalMaterial);
    bool sampleDiffuse = luminRandom01(seed ^ 0x51ed270bu) < diffuseProbability;
    float selectionProbability = sampleDiffuse ? diffuseProbability : 1.0 - diffuseProbability;
    LuminBrdfSample sample = sampleDiffuse
        ? luminSampleCookTorranceDiffuse(
              physicalMaterial, normal, toView,
              float2(luminRandom01(seed ^ 0x9e3779b9u), luminRandom01(seed ^ 0x243f6a88u)))
        : luminSampleCookTorranceSpecular(
              physicalMaterial, normal, toView,
              float2(luminRandom01(seed ^ 0xb7e15162u), luminRandom01(seed ^ 0x94d049bbu)));
    if (sample.valid == 0u || selectionProbability <= 0.0) {
        diffuseRadianceHitDistance[pixel] = float4(0.0);
        specularRadianceHitDistance[pixel] = float4(0.0);
        return;
    }

    IndirectPayload payload = traceIndirect(
        positionSample.xyz + normal * frame.traceParameters.x, sample, !sampleDiffuse, physicalMaterial.roughness);
    float3 estimate = payload.radiance * (sample.throughput / selectionProbability);
    float normHitDistance = REBLUR_FrontEnd_GetNormHitDist(
        payload.hitDistance, primaryViewZ, float3(3.0, 0.1, 20.0), physicalMaterial.roughness);
    float3 materialFactor = sampleDiffuse ? diffuseMaterialFactor : specularMaterialFactor;
    float3 demodulated = estimate / max(materialFactor, float3(1.0e-4));
    float4 packedSignal = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        luminClampFp16Radiance(demodulated), normHitDistance, true);
    diffuseRadianceHitDistance[pixel] = sampleDiffuse ? packedSignal : float4(0.0);
    specularRadianceHitDistance[pixel] = sampleDiffuse ? float4(0.0) : packedSignal;
}

[shader("miss")]
void sharcIndirectLightingRadianceMissMain(inout IndirectPayload payload) {
    payload.radiance = atmosphereSampleEnvironmentRadiance(normalize(WorldRayDirection())) * sharcFrame.sunIrradiance.w;
    payload.hitDistance = frame.traceParameters.y;
}

[shader("miss")]
void sharcIndirectLightingShadowMissMain(inout IndirectPayload payload) {
    payload.visibility = 1.0;
}

[shader("closesthit")]
void sharcIndirectLightingClosestHitMain(
    inout IndirectPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    if (payload.rayKind == kIndirectShadowRay) {
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
    float3 worldNormal = luminOrientShadingNormal(
        mul((float3x3)instance.normalMatrix, objectNormal), WorldRayDirection());
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

    payload.hitDistance = RayTCurrent();
    float3 hitPosition = WorldRayOrigin() + WorldRayDirection() * payload.hitDistance;
    SharcHitData sharcHit;
    sharcHit.positionWorld = hitPosition;
    sharcHit.normalWorld = worldNormal;
    float3 cachedRadiance;
    if (cacheFootprintIsUsable(payload, hitPosition) &&
        SharcGetCachedRadiance(indirectSharcParameters(), sharcHit, cachedRadiance, false)) {
        payload.radiance = cachedRadiance;
        InterlockedAdd(sharcStatistics[kSharcStatisticQueryHit], 1u);
        return;
    }

    uint lightCount = max(frame.samplingParameters.x, 1u);
    uint lightSeed = luminLightRandomSeed(
        DispatchRaysIndex().xy, uint(frame.traceParameters.z), InstanceID(), PrimitiveIndex());
    uint lightIndex = luminSampleLightIndex(luminRandom01(lightSeed), lightCount);
    LuminLightSample lightSample = luminEvaluatePhysicalLight(
        lights[lightIndex], hitPosition, frame.traceParameters.x, frame.traceParameters.y);
    float visibility = lightSample.castsShadows == 0u ? 1.0 : 0.0;
    if (lightSample.valid != 0u && lightSample.castsShadows != 0u &&
        dot(worldNormal, lightSample.direction) > 0.0) {
        RayDesc shadowRay;
        shadowRay.Origin = hitPosition + worldNormal * frame.traceParameters.x;
        shadowRay.Direction = lightSample.direction;
        shadowRay.TMin = frame.traceParameters.x;
        shadowRay.TMax = lightSample.shadowTMax;
        IndirectPayload shadowPayload;
        shadowPayload.radiance = float3(0.0);
        shadowPayload.hitDistance = shadowRay.TMax;
        shadowPayload.rayKind = kIndirectShadowRay;
        shadowPayload.visibility = 0.0;
        shadowPayload.specularPath = 0u;
        shadowPayload.pathRoughness = 1.0;
        TraceRay(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, 0, 1, 1, shadowRay, shadowPayload);
        visibility = shadowPayload.visibility;
    }
    float3 toView = luminPhysicalSafeNormalize(-WorldRayDirection(), worldNormal);
    payload.radiance = luminEvaluateDirectLighting(material, worldNormal, toView, lightSample, visibility) *
                       float(lightCount);
}

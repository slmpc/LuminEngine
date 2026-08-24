import GpuScene;
import PhysicalLighting;
import atmosphere.AtmosphereBindings;

struct RayTracedDiConstants {
    float4x4 inverseViewProjection;
    float4x4 previousViewProjection;
    float4 cameraPosition;
    float4 cameraForward;
    float4 toSunWorld;
    float4 sunIrradiance;
    float4 renderSize;
    float4 traceParameters;
    uint4 samplingParameters;
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
[[vk::binding(14, 0)]] Texture2D<float4> baseColorTextures[];
[[vk::binding(15, 0)]] Texture2D<float4> normalRoughnessTextures[];
[[vk::binding(16, 0)]] SamplerState materialSampler;
[[vk::binding(17, 0)]] StructuredBuffer<GpuLightData> lights;
[[vk::binding(18, 0)]] RWTexture2D<float4> directDiffuseRadianceHitT;
[[vk::binding(19, 0)]] RWTexture2D<float4> directSpecularRadianceHitT;

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

void writeMiss(uint2 pixel, float3 environmentRadiance) {
    worldPositionHitT[pixel] = float4(0.0, 0.0, 0.0, -1.0);
    normalRoughness[pixel] = float4(0.0, 0.0, 0.0, 1.0);
    albedoMetallic[pixel] = float4(0.0);
    materialIdOutput[pixel] = kInvalidMaterial;
    viewZ[pixel] = 0.0;
    motion[pixel] = float2(0.0);
    directRadiance[pixel] = float4(luminClampFp16Radiance(environmentRadiance), 1.0);
    directDiffuseRadianceHitT[pixel] = float4(0.0);
    directSpecularRadianceHitT[pixel] = float4(0.0);
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
    TraceRay(sceneTlas, 0, 0xff, 0, 1, 0, ray, payload);
}

[shader("miss")]
void primaryMissMain(inout PrimaryPayload payload) {
    if (payload.rayKind == kShadowRay) {
        payload.visibility = 1u;
        return;
    }
    uint2 pixel = DispatchRaysIndex().xy;
    writeMiss(pixel, atmosphereSamplePrimaryEnvironmentRadiance(WorldRayDirection()) * frame.sunIrradiance.w);
}

[shader("miss")]
void shadowMissMain(inout PrimaryPayload payload) {
    payload.visibility = 1u;
}

[shader("closesthit")]
void primaryClosestHitMain(inout PrimaryPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
    if (payload.rayKind == kShadowRay) {
        payload.visibility = 0u;
        payload.hitDistance = RayTCurrent();
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
    GpuPackedVertex vertex0 = vertices[vertexIndices.x + instance.metadata.z];
    GpuPackedVertex vertex1 = vertices[vertexIndices.y + instance.metadata.z];
    GpuPackedVertex vertex2 = vertices[vertexIndices.z + instance.metadata.z];
    float3 objectNormal = normalize(vertex0.normal.xyz * barycentrics.x + vertex1.normal.xyz * barycentrics.y +
                                    vertex2.normal.xyz * barycentrics.z);
    float3 normal = luminOrientShadingNormal(mul((float3x3)instance.normalMatrix, objectNormal),
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
    float3 toView = safeNormalize(frame.cameraPosition.xyz - hitPosition, normal);
    uint lightCount = max(frame.samplingParameters.x, 1u);
    uint lightSeed = luminLightRandomSeed(pixel, frame.samplingParameters.y, InstanceID(), PrimitiveIndex());
    uint lightIndex = luminSampleLightIndex(luminRandom01(lightSeed), lightCount);
    LuminLightSample lightSample = luminEvaluatePhysicalLight(
        lights[lightIndex], hitPosition, frame.traceParameters.x, frame.traceParameters.y);
    float noL = dot(normal, lightSample.direction);
    float visibility = lightSample.castsShadows == 0u ? 1.0 : 0.0;
    // NRD 把零命中距离视为当前波瓣没有有效样本；背光面不能伪装成一次远距离未遮挡命中。
    float shadowHitDistance = lightSample.valid != 0u && noL > 0.0 ? lightSample.shadowTMax : 0.0;
    if (lightSample.valid != 0u && lightSample.castsShadows != 0u && frame.traceParameters.z >= 0.5 &&
        noL > 0.0) {
        RayDesc shadowRay;
        shadowRay.Origin = hitPosition + normal * frame.traceParameters.x;
        shadowRay.Direction = lightSample.direction;
        shadowRay.TMin = frame.traceParameters.x;
        shadowRay.TMax = lightSample.shadowTMax;
        PrimaryPayload shadowPayload;
        shadowPayload.radiance = float3(0.0);
        shadowPayload.hitDistance = shadowRay.TMax;
        shadowPayload.normal = float3(0.0);
        shadowPayload.materialId = kInvalidMaterial;
        shadowPayload.rayKind = kShadowRay;
        shadowPayload.hit = 0u;
        shadowPayload.visibility = 0u;
        shadowPayload.padding = 0u;
        TraceRay(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, 0, 1, 1, shadowRay, shadowPayload);
        visibility = float(shadowPayload.visibility);
        shadowHitDistance = shadowPayload.hitDistance;
    }

    LuminCookTorranceLobes directLobes;
    directLobes.diffuse = float3(0.0);
    directLobes.specular = float3(0.0);
    if (lightSample.valid != 0u && frame.traceParameters.z >= 0.5) {
        directLobes = luminEvaluateDirectLightingLobes(material, normal, toView, lightSample, visibility);
        directLobes.diffuse *= float(lightCount);
        directLobes.specular *= float(lightCount);
    }
    float3 direct = directLobes.diffuse + directLobes.specular;
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
    // Cook-Torrance 的低粗糙度掠射峰值可超过 half 上限；仅在 FP16 scene-radiance 存储边界截断，避免 Inf 污染 TAA。
    directRadiance[pixel] = float4(luminClampFp16Radiance(direct), 1.0);
    float storedShadowHitDistance = clamp(shadowHitDistance, 0.0, kLuminFp16Max);
    directDiffuseRadianceHitT[pixel] =
        float4(luminClampFp16Radiance(directLobes.diffuse), storedShadowHitDistance);
    directSpecularRadianceHitT[pixel] =
        float4(luminClampFp16Radiance(directLobes.specular), storedShadowHitDistance);
    visibilityMask[pixel] = 1u;
}

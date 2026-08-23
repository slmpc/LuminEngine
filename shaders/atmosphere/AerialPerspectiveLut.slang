import AtmosphereCommon;
import AtmosphereUniforms;

[[vk::binding(0, 0)]] ConstantBuffer<AtmosphereGpuConstants> atmosphere;
[[vk::binding(1, 0)]] Texture2D<float4> transmittanceLut;
[[vk::binding(2, 0)]] Texture2D<float4> multiScatteringLut;
[[vk::binding(3, 0)]] SamplerState atmosphereSampler;
[[vk::binding(4, 0)]] RWTexture3D<float4> aerialPerspectiveOutput;

static const uint kAerialPerspectiveStepCount = 24;

[shader("compute")]
[numthreads(4, 4, 4)]
void aerialPerspectiveMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width;
    uint height;
    uint depth;
    aerialPerspectiveOutput.GetDimensions(width, height, depth);
    if (any(dispatchThreadId >= uint3(width, height, depth))) {
        return;
    }

    if (atmosphere.groundAlbedoAndEnabled.w < 0.5) {
        aerialPerspectiveOutput[dispatchThreadId] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float2 screenUnit = float2(atmosphereUnitFromTexel(dispatchThreadId.x, width),
                               atmosphereUnitFromTexel(dispatchThreadId.y, height));
    float depthUnit = atmosphereUnitFromTexel(dispatchThreadId.z, depth);
    float nearKm = atmosphere.worldMappingAndClipKm.z;
    float farKm = atmosphere.worldMappingAndClipKm.w;
    float requestedDistanceKm = lerp(nearKm, farKm, depthUnit * depthUnit);
    float3 rayDirection = atmosphereViewDirectionFromUnit(screenUnit, atmosphere);
    float3 originKm = atmosphereLocalCameraOriginKm(atmosphere);
    float3 toSun = normalize(atmosphere.toSunWorld.xyz);
    float3 solarIrradiance = atmosphereSolarIrradiance(atmosphere);
    AtmosphereBoundaryDistance boundary = atmosphereDistanceToBoundary(originKm, rayDirection, atmosphere);
    float integrationDistanceKm =
        min(max(requestedDistanceKm - boundary.startDistanceKm, 0.0), boundary.distanceKm);

    float3 pathTransmittance = float3(1.0);
    float3 inScattering = float3(0.0);
    float phaseCosine = dot(rayDirection, toSun);
    float rayleighPhase = atmosphereRayleighPhase(phaseCosine);
    float miePhase = atmosphereMiePhase(phaseCosine, atmosphere.mieAbsorptionAndPhaseG.w);
    float stepKm = integrationDistanceKm / float(kAerialPerspectiveStepCount);
    for (uint sampleIndex = 0; sampleIndex < kAerialPerspectiveStepCount; ++sampleIndex) {
        float distanceAlongRayKm =
            boundary.startDistanceKm + (float(sampleIndex) + 0.5) * stepKm;
        float3 samplePositionKm = originKm + rayDirection * distanceAlongRayKm;
        AtmosphereMediumSample medium = atmosphereSampleMedium(samplePositionKm, atmosphere);
        float3 segmentTransmittance = exp(-min(medium.extinction * stepKm, float3(80.0)));
        float3 integrationWeight =
            pathTransmittance * atmosphereIntegrateConstantSource(medium.extinction, stepKm);
        float3 transmittanceToSun = atmosphereSampleTransmittanceToSun(
            transmittanceLut, atmosphereSampler, samplePositionKm, toSun, atmosphere);
        float3 directSource = transmittanceToSun *
                              (medium.rayleighScattering * rayleighPhase + medium.mieScattering * miePhase);
        float3 multipleScattering = atmosphereSampleMultiScattering(
            multiScatteringLut, atmosphereSampler, samplePositionKm, toSun, atmosphere);
        float3 multipleSource = multipleScattering *
                                (medium.rayleighScattering + medium.mieScattering);
        inScattering += integrationWeight * solarIrradiance * (directSource + multipleSource);
        pathTransmittance *= segmentTransmittance;
    }

    // RGB 保存相机到该深度的累计 in-scattering，A 保存 RGB 透射率的平均值。
    aerialPerspectiveOutput[dispatchThreadId] =
        float4(max(inScattering, float3(0.0)), saturate(atmosphereAverage(pathTransmittance)));
}

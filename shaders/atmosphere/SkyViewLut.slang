import AtmosphereCommon;
import AtmosphereUniforms;

[[vk::binding(0, 0)]] ConstantBuffer<AtmosphereGpuConstants> atmosphere;
[[vk::binding(1, 0)]] Texture2D<float4> transmittanceLut;
[[vk::binding(2, 0)]] Texture2D<float4> multiScatteringLut;
[[vk::binding(3, 0)]] SamplerState atmosphereSampler;
[[vk::binding(4, 0)]] RWTexture2D<float4> skyViewOutput;

static const uint kSkyViewStepCount = 48;

[shader("compute")]
[numthreads(8, 8, 1)]
void skyViewMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width;
    uint height;
    skyViewOutput.GetDimensions(width, height);
    if (any(dispatchThreadId.xy >= uint2(width, height))) {
        return;
    }

    if (atmosphere.groundAlbedoAndEnabled.w < 0.5) {
        skyViewOutput[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float2 unitCoordinate = float2(atmosphereUnitFromTexel(dispatchThreadId.x, width),
                                   atmosphereUnitFromTexel(dispatchThreadId.y, height));
    float3 rayDirection = atmosphereSkyDirectionFromUnit(unitCoordinate);
    float3 originKm = atmosphereLocalCameraOriginKm(atmosphere);
    float3 toSun = normalize(atmosphere.toSunWorld.xyz);
    float3 solarIrradiance = atmosphereSolarIrradiance(atmosphere);
    AtmosphereBoundaryDistance boundary = atmosphereDistanceToBoundary(originKm, rayDirection, atmosphere);

    float3 pathTransmittance = float3(1.0);
    float3 inScattering = float3(0.0);
    float phaseCosine = dot(rayDirection, toSun);
    float rayleighPhase = atmosphereRayleighPhase(phaseCosine);
    float miePhase = atmosphereMiePhase(phaseCosine, atmosphere.mieAbsorptionAndPhaseG.w);
    for (uint sampleIndex = 0; sampleIndex < kSkyViewStepCount; ++sampleIndex) {
        // 二次分布在相机附近给出更多样本，同时仍覆盖完整边界距离。
        float segmentStartUnit = float(sampleIndex) / float(kSkyViewStepCount);
        float segmentEndUnit = float(sampleIndex + 1) / float(kSkyViewStepCount);
        float segmentStartKm = boundary.distanceKm * segmentStartUnit * segmentStartUnit;
        float segmentEndKm = boundary.distanceKm * segmentEndUnit * segmentEndUnit;
        float stepKm = segmentEndKm - segmentStartKm;
        float distanceAlongRayKm =
            boundary.startDistanceKm + 0.5 * (segmentStartKm + segmentEndKm);
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

    if (boundary.hitsGround) {
        float3 groundPositionKm =
            originKm + rayDirection * (boundary.startDistanceKm + boundary.distanceKm);
        float3 groundNormal = normalize(groundPositionKm);
        float sunCosine = saturate(dot(groundNormal, toSun));
        float3 groundTransmittance = atmosphereSampleTransmittanceToSun(
            transmittanceLut, atmosphereSampler, groundPositionKm + groundNormal * kAtmosphereRayEpsilonKm,
            toSun, atmosphere);
        float3 groundRadiance = atmosphere.groundAlbedoAndEnabled.rgb * solarIrradiance *
                                groundTransmittance * (sunCosine / kAtmospherePi);
        inScattering += pathTransmittance * groundRadiance;
    }

    skyViewOutput[dispatchThreadId.xy] =
        float4(max(inScattering, float3(0.0)), saturate(atmosphereAverage(pathTransmittance)));
}

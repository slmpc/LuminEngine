#include "AtmosphereCommon.slang"

[[vk::binding(0, 0)]] ConstantBuffer<AtmosphereGpuConstants> atmosphere;
[[vk::binding(1, 0)]] Texture2D<float4> transmittanceLut;
[[vk::binding(3, 0)]] SamplerState atmosphereSampler;
[[vk::binding(4, 0)]] RWTexture2D<float4> multiScatteringOutput;

static const uint kMultiScatteringDirectionCount = 32;
static const uint kMultiScatteringStepCount = 16;
static const float kMultiScatteringGoldenAngle = 2.39996322972865332;

[shader("compute")]
[numthreads(8, 8, 1)]
void multiScatteringMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width;
    uint height;
    multiScatteringOutput.GetDimensions(width, height);
    if (any(dispatchThreadId.xy >= uint2(width, height))) {
        return;
    }

    if (atmosphere.groundAlbedoAndEnabled.w < 0.5) {
        multiScatteringOutput[dispatchThreadId.xy] = float4(0.0);
        return;
    }

    float2 unitCoordinate = float2(atmosphereUnitFromTexel(dispatchThreadId.x, width),
                                   atmosphereUnitFromTexel(dispatchThreadId.y, height));
    float bottomRadiusKm = atmosphere.atmosphereRadiiKm.x;
    float topRadiusKm = atmosphere.atmosphereRadiiKm.y;
    float radiusKm = lerp(bottomRadiusKm + kAtmosphereRayEpsilonKm,
                          topRadiusKm - kAtmosphereRayEpsilonKm, unitCoordinate.y);
    float sunZenithCosine = unitCoordinate.x * 2.0 - 1.0;
    float3 originKm = float3(0.0, radiusKm, 0.0);
    float3 toSun = normalize(float3(atmosphereSafeSqrt(1.0 - sunZenithCosine * sunZenithCosine),
                                    sunZenithCosine, 0.0));

    float3 averagedDirectRadiance = float3(0.0);
    float3 averagedScatteringTransfer = float3(0.0);
    for (uint directionIndex = 0; directionIndex < kMultiScatteringDirectionCount; ++directionIndex) {
        // Fibonacci 球面点集不依赖 subgroup 大小，且每次生成结果完全确定。
        float sphereY = 1.0 - 2.0 * (float(directionIndex) + 0.5) / float(kMultiScatteringDirectionCount);
        float sphereRadius = atmosphereSafeSqrt(1.0 - sphereY * sphereY);
        float azimuth = kMultiScatteringGoldenAngle * float(directionIndex);
        float3 rayDirection = float3(sphereRadius * cos(azimuth), sphereY, sphereRadius * sin(azimuth));
        AtmosphereBoundaryDistance boundary = atmosphereDistanceToBoundary(originKm, rayDirection, atmosphere);
        float stepKm = boundary.distanceKm / float(kMultiScatteringStepCount);
        float3 pathTransmittance = float3(1.0);
        float3 directRadiance = float3(0.0);
        float3 scatteringTransfer = float3(0.0);
        float phaseCosine = dot(rayDirection, toSun);
        float rayleighPhase = atmosphereRayleighPhase(phaseCosine);
        float miePhase = atmosphereMiePhase(phaseCosine, atmosphere.mieAbsorptionAndPhaseG.w);

        for (uint sampleIndex = 0; sampleIndex < kMultiScatteringStepCount; ++sampleIndex) {
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
            directRadiance += integrationWeight * directSource;
            scatteringTransfer += integrationWeight * (medium.rayleighScattering + medium.mieScattering);
            pathTransmittance *= segmentTransmittance;
        }

        if (boundary.hitsGround) {
            float3 groundPositionKm =
                originKm + rayDirection * (boundary.startDistanceKm + boundary.distanceKm);
            float3 groundNormal = normalize(groundPositionKm);
            float sunCosine = saturate(dot(groundNormal, toSun));
            float3 groundTransmittance = atmosphereSampleTransmittanceToSun(
                transmittanceLut, atmosphereSampler,
                groundPositionKm + groundNormal * kAtmosphereRayEpsilonKm, toSun, atmosphere);
            directRadiance += pathTransmittance * atmosphere.groundAlbedoAndEnabled.rgb *
                              (sunCosine / kAtmospherePi) * groundTransmittance;
            scatteringTransfer += pathTransmittance * atmosphere.groundAlbedoAndEnabled.rgb;
        }

        averagedDirectRadiance += directRadiance;
        averagedScatteringTransfer += scatteringTransfer;
    }

    averagedDirectRadiance /= float(kMultiScatteringDirectionCount);
    averagedScatteringTransfer =
        saturate(averagedScatteringTransfer / float(kMultiScatteringDirectionCount));
    float3 multipleScattering =
        averagedDirectRadiance / max(float3(1.0) - averagedScatteringTransfer, float3(0.05));
    multiScatteringOutput[dispatchThreadId.xy] =
        float4(max(multipleScattering, float3(0.0)), atmosphereAverage(averagedScatteringTransfer));
}

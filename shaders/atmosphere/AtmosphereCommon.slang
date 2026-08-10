#ifndef LUMIN_ATMOSPHERE_COMMON_SLANG
#define LUMIN_ATMOSPHERE_COMMON_SLANG

#include "AtmosphereUniforms.slang"

// 本实现依据 Hillaire EGSR 2020 与 Bruneton 公开论文中的解析公式独立编写。
// 仅参考公开的物理模型和参数化，未复制、翻译或改写 Alpha-Piscium 的 GPLv3 shader 源码。
static const float kAtmospherePi = 3.14159265358979323846;
static const float kAtmosphereRayEpsilonKm = 1.0e-3;
// 当前 renderer 尚未使用物理相机曝光；该比例把 lux 映射到现有 HDR 辐射范围，接入物理曝光后应统一移除。
static const float kAtmosphereLuxToRendererRadiance = 1.0e-5;

struct AtmosphereMediumSample {
    float3 rayleighScattering;
    float3 mieScattering;
    float3 extinction;
};

struct AtmosphereBoundaryDistance {
    float startDistanceKm;
    float distanceKm;
    bool hitsGround;
};

float atmosphereSafeSqrt(float value) {
    return sqrt(max(value, 0.0));
}

float atmosphereAverage(float3 value) {
    return dot(value, float3(1.0 / 3.0));
}

float2 atmosphereRaySphereRoots(float3 originKm, float3 direction, float radiusKm) {
    float projected = dot(originKm, direction);
    float discriminant = projected * projected - dot(originKm, originKm) + radiusKm * radiusKm;
    if (discriminant < 0.0) {
        return float2(-1.0);
    }

    float root = sqrt(discriminant);
    return float2(-projected - root, -projected + root);
}

bool atmosphereRayIntersectsGround(float3 originKm, float3 direction, AtmosphereGpuConstants atmosphere) {
    float bottomRadiusKm = atmosphere.atmosphereRadiiKm.x;
    float radiusKm = length(originKm);
    if (radiusKm <= bottomRadiusKm + kAtmosphereRayEpsilonKm && dot(originKm, direction) < 0.0) {
        return true;
    }

    float2 roots = atmosphereRaySphereRoots(originKm, direction, bottomRadiusKm);
    return roots.x > kAtmosphereRayEpsilonKm || roots.y > kAtmosphereRayEpsilonKm;
}

AtmosphereBoundaryDistance atmosphereDistanceToBoundary(
    float3 originKm, float3 direction, AtmosphereGpuConstants atmosphere) {
    float bottomRadiusKm = atmosphere.atmosphereRadiiKm.x;
    float topRadiusKm = atmosphere.atmosphereRadiiKm.y;
    float2 topRoots = atmosphereRaySphereRoots(originKm, direction, topRadiusKm);

    AtmosphereBoundaryDistance result;
    result.startDistanceKm = 0.0;
    result.distanceKm = 0.0;
    result.hitsGround = false;
    if (topRoots.y <= kAtmosphereRayEpsilonKm) {
        return result;
    }

    float radiusKm = length(originKm);
    float segmentStartKm = radiusKm >= topRadiusKm ? max(topRoots.x, 0.0) : 0.0;
    float segmentEndKm = topRoots.y;

    if (radiusKm <= bottomRadiusKm + kAtmosphereRayEpsilonKm && dot(originKm, direction) < 0.0) {
        result.hitsGround = true;
        return result;
    }

    float2 groundRoots = atmosphereRaySphereRoots(originKm, direction, bottomRadiusKm);
    float groundDistanceKm = groundRoots.x > segmentStartKm + 1.0e-6 ? groundRoots.x : groundRoots.y;
    if (groundDistanceKm > segmentStartKm + 1.0e-6 && groundDistanceKm < segmentEndKm) {
        segmentEndKm = groundDistanceKm;
        result.hitsGround = true;
    }

    result.startDistanceKm = segmentStartKm;
    result.distanceKm = max(segmentEndKm - segmentStartKm, 0.0);
    return result;
}

float3 atmosphereLocalCameraOriginKm(AtmosphereGpuConstants atmosphere) {
    // LUT 在相机处使用 Y-up 局部切平面。水平平移不会改变球对称介质，也无需让 sky-view 重建。
    float minimumRadiusKm = atmosphere.atmosphereRadiiKm.x + kAtmosphereRayEpsilonKm;
    // 高于大气顶层的相机保持真实高度，由球壳求交裁剪真空段；只有地下视点需要抬到地表外侧。
    float radiusKm = max(atmosphere.cameraPlanetPositionKm.y, minimumRadiusKm);
    return float3(0.0, radiusKm, 0.0);
}

AtmosphereMediumSample atmosphereSampleMedium(float3 positionKm, AtmosphereGpuConstants atmosphere) {
    float altitudeKm = max(length(positionKm) - atmosphere.atmosphereRadiiKm.x, 0.0);
    float rayleighDensity = exp(-altitudeKm * atmosphere.rayleighScatteringAndInvScaleHeight.w);
    float mieDensity = exp(-altitudeKm * atmosphere.mieScatteringAndInvScaleHeight.w);
    float ozoneDensity = saturate(
        1.0 - abs(altitudeKm - atmosphere.ozoneDensityProfileKm.x) * atmosphere.ozoneDensityProfileKm.z);

    AtmosphereMediumSample sample;
    sample.rayleighScattering = atmosphere.rayleighScatteringAndInvScaleHeight.rgb * rayleighDensity;
    sample.mieScattering = atmosphere.mieScatteringAndInvScaleHeight.rgb * mieDensity;
    float3 mieExtinction =
        (atmosphere.mieScatteringAndInvScaleHeight.rgb + atmosphere.mieAbsorptionAndPhaseG.rgb) * mieDensity;
    sample.extinction = sample.rayleighScattering + mieExtinction + atmosphere.ozoneAbsorptionPerKm.rgb * ozoneDensity;
    return sample;
}

float atmosphereRayleighPhase(float cosTheta) {
    return (3.0 / (16.0 * kAtmospherePi)) * (1.0 + cosTheta * cosTheta);
}

float atmosphereMiePhase(float cosTheta, float phaseG) {
    // Cornette-Shanks 相位函数；g 已由 CPU 验证为严格位于 (-1, 1)。
    float g2 = phaseG * phaseG;
    float denominator = max(1.0 + g2 - 2.0 * phaseG * cosTheta, 1.0e-4);
    float normalization = (3.0 / (8.0 * kAtmospherePi)) * (1.0 - g2) / (2.0 + g2);
    return normalization * (1.0 + cosTheta * cosTheta) / pow(denominator, 1.5);
}

float3 atmosphereIntegrateTransmittance(
    float3 originKm, float3 direction, float distanceKm, uint sampleCount, AtmosphereGpuConstants atmosphere) {
    if (distanceKm <= 0.0 || sampleCount == 0) {
        return float3(1.0);
    }

    float stepKm = distanceKm / float(sampleCount);
    float3 opticalDepth = float3(0.0);
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float distanceAlongRayKm = (float(sampleIndex) + 0.5) * stepKm;
        AtmosphereMediumSample medium = atmosphereSampleMedium(originKm + direction * distanceAlongRayKm, atmosphere);
        opticalDepth += medium.extinction * stepKm;
    }
    return exp(-min(opticalDepth, float3(80.0)));
}

float atmosphereIntegrateConstantSourceChannel(float extinctionPerKm, float distanceKm) {
    float safeDistanceKm = max(distanceKm, 0.0);
    float safeExtinctionPerKm = max(extinctionPerKm, 0.0);
    if (safeExtinctionPerKm <= 1.0e-6) {
        return safeDistanceKm;
    }

    // 在单个步长内假定介质与入射源恒定，解析积分比梯形权重在长光学路径上更稳定。
    float opticalDepth = min(safeExtinctionPerKm * safeDistanceKm, 80.0);
    return (1.0 - exp(-opticalDepth)) / safeExtinctionPerKm;
}

float3 atmosphereIntegrateConstantSource(float3 extinctionPerKm, float distanceKm) {
    return float3(
        atmosphereIntegrateConstantSourceChannel(extinctionPerKm.x, distanceKm),
        atmosphereIntegrateConstantSourceChannel(extinctionPerKm.y, distanceKm),
        atmosphereIntegrateConstantSourceChannel(extinctionPerKm.z, distanceKm));
}

float atmosphereUnitFromTexel(uint index, uint extent) {
    return extent > 1 ? float(index) / float(extent - 1) : 0.5;
}

float atmosphereTexelCenterFromUnit(float unitCoordinate, uint extent) {
    return extent > 1 ? (saturate(unitCoordinate) * float(extent - 1) + 0.5) / float(extent) : 0.5;
}

float2 atmosphereTransmittanceUnitFromPhysical(
    float radiusKm, float viewZenithCosine, AtmosphereGpuConstants atmosphere) {
    float bottomRadiusKm = atmosphere.atmosphereRadiiKm.x;
    float topRadiusKm = atmosphere.atmosphereRadiiKm.y;
    float heightKm = atmosphereSafeSqrt(topRadiusKm * topRadiusKm - bottomRadiusKm * bottomRadiusKm);
    float rhoKm = atmosphereSafeSqrt(radiusKm * radiusKm - bottomRadiusKm * bottomRadiusKm);
    float discriminant = radiusKm * radiusKm * (viewZenithCosine * viewZenithCosine - 1.0) +
                         topRadiusKm * topRadiusKm;
    float distanceKm = max(-radiusKm * viewZenithCosine + atmosphereSafeSqrt(discriminant), 0.0);
    float minimumDistanceKm = topRadiusKm - radiusKm;
    float maximumDistanceKm = rhoKm + heightKm;
    float distanceRangeKm = max(maximumDistanceKm - minimumDistanceKm, kAtmosphereRayEpsilonKm);
    return saturate(float2((distanceKm - minimumDistanceKm) / distanceRangeKm, rhoKm / heightKm));
}

void atmosphereTransmittancePhysicalFromUnit(
    float2 unitCoordinate, AtmosphereGpuConstants atmosphere, out float radiusKm, out float viewZenithCosine) {
    float bottomRadiusKm = atmosphere.atmosphereRadiiKm.x;
    float topRadiusKm = atmosphere.atmosphereRadiiKm.y;
    float heightKm = atmosphereSafeSqrt(topRadiusKm * topRadiusKm - bottomRadiusKm * bottomRadiusKm);
    float rhoKm = heightKm * saturate(unitCoordinate.y);
    radiusKm = atmosphereSafeSqrt(rhoKm * rhoKm + bottomRadiusKm * bottomRadiusKm);

    float minimumDistanceKm = topRadiusKm - radiusKm;
    float maximumDistanceKm = rhoKm + heightKm;
    float distanceKm = lerp(minimumDistanceKm, maximumDistanceKm, saturate(unitCoordinate.x));
    if (distanceKm <= kAtmosphereRayEpsilonKm) {
        viewZenithCosine = 1.0;
    } else {
        viewZenithCosine = clamp(
            (heightKm * heightKm - rhoKm * rhoKm - distanceKm * distanceKm) /
                (2.0 * radiusKm * distanceKm),
            -1.0, 1.0);
    }
}

float3 atmosphereSampleTransmittanceToSun(
    Texture2D<float4> transmittanceLut, SamplerState lutSampler, float3 positionKm, float3 toSun,
    AtmosphereGpuConstants atmosphere) {
    if (atmosphereRayIntersectsGround(positionKm, toSun, atmosphere)) {
        return float3(0.0);
    }

    uint width;
    uint height;
    transmittanceLut.GetDimensions(width, height);
    float radiusKm = clamp(length(positionKm), atmosphere.atmosphereRadiiKm.x, atmosphere.atmosphereRadiiKm.y);
    float viewZenithCosine = dot(positionKm, toSun) / max(radiusKm, 1.0e-4);
    float2 unitCoordinate = atmosphereTransmittanceUnitFromPhysical(radiusKm, viewZenithCosine, atmosphere);
    float2 uv = float2(atmosphereTexelCenterFromUnit(unitCoordinate.x, width),
                       atmosphereTexelCenterFromUnit(unitCoordinate.y, height));
    return max(transmittanceLut.SampleLevel(lutSampler, uv, 0.0).rgb, float3(0.0));
}

float2 atmosphereMultiScatteringUnitFromPhysical(
    float radiusKm, float sunZenithCosine, AtmosphereGpuConstants atmosphere) {
    float altitudeUnit = (radiusKm - atmosphere.atmosphereRadiiKm.x) * atmosphere.atmosphereRadiiKm.w;
    return saturate(float2(sunZenithCosine * 0.5 + 0.5, altitudeUnit));
}

float3 atmosphereSampleMultiScattering(
    Texture2D<float4> multiScatteringLut, SamplerState lutSampler, float3 positionKm, float3 toSun,
    AtmosphereGpuConstants atmosphere) {
    uint width;
    uint height;
    multiScatteringLut.GetDimensions(width, height);
    float radiusKm = length(positionKm);
    float sunZenithCosine = dot(positionKm, toSun) / max(radiusKm, 1.0e-4);
    float2 unitCoordinate = atmosphereMultiScatteringUnitFromPhysical(radiusKm, sunZenithCosine, atmosphere);
    float2 uv = float2(atmosphereTexelCenterFromUnit(unitCoordinate.x, width),
                       atmosphereTexelCenterFromUnit(unitCoordinate.y, height));
    return max(multiScatteringLut.SampleLevel(lutSampler, uv, 0.0).rgb, float3(0.0));
}

float3 atmosphereSolarIrradiance(AtmosphereGpuConstants atmosphere) {
    return max(atmosphere.sunColorIlluminanceLux.rgb, float3(0.0)) * max(atmosphere.sunColorIlluminanceLux.w, 0.0) *
           kAtmosphereLuxToRendererRadiance;
}

float3 atmosphereSkyDirectionFromUnit(float2 unitCoordinate) {
    float longitude = (unitCoordinate.x - 0.5) * (2.0 * kAtmospherePi);
    // 对称平方映射把更多 texel 分配给地平线；它来自 Hillaire 2020 的公开 sky-view 参数化。
    float signedHorizonCoordinate = 1.0 - 2.0 * saturate(unitCoordinate.y);
    float latitude = sign(signedHorizonCoordinate) * signedHorizonCoordinate * signedHorizonCoordinate *
                     (0.5 * kAtmospherePi);
    float cosLatitude = cos(latitude);
    return normalize(float3(cosLatitude * sin(longitude), sin(latitude), -cosLatitude * cos(longitude)));
}

float2 atmosphereSkyUnitFromDirection(float3 direction) {
    direction = normalize(direction);
    float longitudeUnit = atan2(direction.x, -direction.z) / (2.0 * kAtmospherePi) + 0.5;
    float latitude = asin(clamp(direction.y, -1.0, 1.0));
    float signedHorizonCoordinate = sign(latitude) * sqrt(abs(latitude) / (0.5 * kAtmospherePi));
    float latitudeUnit = 0.5 - 0.5 * signedHorizonCoordinate;
    return float2(frac(longitudeUnit), saturate(latitudeUnit));
}

float3 atmosphereViewDirectionFromUnit(float2 unitCoordinate, AtmosphereGpuConstants atmosphere) {
    float2 clipPosition = float2(unitCoordinate.x * 2.0 - 1.0, 1.0 - unitCoordinate.y * 2.0);
    float4 worldFar = mul(atmosphere.inverseViewProjection, float4(clipPosition, 1.0, 1.0));
    float3 worldPosition = worldFar.xyz / max(abs(worldFar.w), 1.0e-6) * sign(worldFar.w);
    return normalize(worldPosition - atmosphere.cameraPositionWorld.xyz);
}

#endif

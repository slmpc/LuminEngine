#ifndef LUMIN_ATMOSPHERE_BINDINGS_SLANG
#define LUMIN_ATMOSPHERE_BINDINGS_SLANG

import AtmosphereCommon;
import AtmosphereUniforms;

// descriptor set 2 是 raster sky、RT miss 与 GI 共用的大气只读 ABI。
[[vk::binding(0, 2)]] ConstantBuffer<AtmosphereGpuConstants> atmosphere;
[[vk::binding(1, 2)]] Texture2D<float4> atmosphereTransmittanceLut;
[[vk::binding(2, 2)]] Texture2D<float4> atmosphereMultiScatteringLut;
[[vk::binding(3, 2)]] Texture2D<float4> atmosphereSkyViewLut;
[[vk::binding(4, 2)]] Texture3D<float4> atmosphereAerialPerspectiveLut;
[[vk::binding(5, 2)]] SamplerState atmosphereSampler;

float4 atmosphereSampleSkyView(float3 worldDirection) {
    uint width;
    uint height;
    atmosphereSkyViewLut.GetDimensions(width, height);
    float2 unitCoordinate = atmosphereSkyUnitFromDirection(worldDirection);
    float2 uv = float2(atmosphereTexelCenterFromUnit(unitCoordinate.x, width),
                       atmosphereTexelCenterFromUnit(unitCoordinate.y, height));
    return atmosphereSkyViewLut.SampleLevel(atmosphereSampler, uv, 0.0);
}

float3 atmosphereSampleEnvironmentRadiance(float3 worldDirection) {
    worldDirection = normalize(worldDirection);
    if (atmosphere.groundAlbedoAndEnabled.w >= 0.5) {
        return atmosphereSampleSkyView(worldDirection).rgb;
    }

    // 禁用物理大气时，所有 raster/RT/GI consumer 仍共享同一套确定性程序化天空。
    float horizon = saturate(worldDirection.y * 0.5 + 0.5);
    float3 horizonColor = float3(0.36, 0.55, 0.78);
    float3 zenithColor = float3(0.055, 0.16, 0.36);
    float3 nadirColor = float3(0.025, 0.03, 0.04);
    return worldDirection.y >= 0.0
        ? lerp(horizonColor, zenithColor, pow(horizon, 0.55))
        : lerp(horizonColor, nadirColor, saturate(-worldDirection.y * 3.0));
}

#endif

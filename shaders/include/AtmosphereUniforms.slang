#ifndef LUMIN_ATMOSPHERE_UNIFORMS_SLANG
#define LUMIN_ATMOSPHERE_UNIFORMS_SLANG

// 大气 compute、raster sky 与 ray tracing 共享的 CPU/GPU ABI。
// 字段顺序必须与 lumin::render::atmosphere::AtmosphereGpuConstants 完全一致。
struct AtmosphereGpuConstants {
    float4x4 view;
    float4x4 projection;
    float4x4 inverseView;
    float4x4 inverseProjection;
    float4x4 inverseViewProjection;
    float4 cameraPositionWorld;
    float4 cameraPlanetPositionKm;
    float4 toSunWorld;
    float4 sunColorIlluminanceLux;
    float4 atmosphereRadiiKm;
    float4 worldMappingAndClipKm;
    float4 renderExtent;
    float4 rayleighScatteringAndInvScaleHeight;
    float4 mieScatteringAndInvScaleHeight;
    float4 mieAbsorptionAndPhaseG;
    float4 ozoneAbsorptionPerKm;
    float4 ozoneDensityProfileKm;
    float4 groundAlbedoAndEnabled;
};

#endif

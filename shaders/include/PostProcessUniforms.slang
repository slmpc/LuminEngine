#ifndef LUMIN_POST_PROCESS_UNIFORMS_SLANG
#define LUMIN_POST_PROCESS_UNIFORMS_SLANG

// 全屏渲染通道共享的 CPU/GPU ABI。字段顺序和偏移由 shader manifest 与 ABI 测试共同锁定。
struct PostProcessUniforms {
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4x4 cascadeViewProjections[4];
    float4 cascadeSplits;
    float4 cameraPosition;
    float4 cameraForward;
    float4 lightDirection; // xyz = world-space sun direction, w = direct-lighting feature enabled
    float4 renderSize;
    float4 renderOptions;
    float4 tonemapOptions; // x = exposure, y = sRGB target, z = post-TAA FSR1 RCAS sharpness
    float4 ambientOcclusionOptions; // x = algorithm, y = world radius, z = strength, w = geometric bias
};

#endif

#ifndef LUMIN_SHARC_RUNTIME_SLANG
#define LUMIN_SHARC_RUNTIME_SLANG

#ifndef LUMIN_SHARC_UPDATE
#define LUMIN_SHARC_UPDATE 0
#endif

// Vulkan 路径固定使用 32-bit lock，避免要求 shaderBufferInt64Atomics。
#define SHARC_ENABLE_GLSL 0
#define SHARC_ENABLE_64_BIT_ATOMICS 0
#define SHARC_USE_FP16 0
#define SHARC_UPDATE LUMIN_SHARC_UPDATE
#define SHARC_QUERY (1 - LUMIN_SHARC_UPDATE)
#include "SharcCommon.h"

struct SharcGpuConstants {
    float4 cameraPositionSceneScale;
    float4 previousCameraPositionLogarithmBase;
    float4 toSunWorldRadianceScale;
    float4 sunIrradiance;
    float4 traceParameters;
    uint4 cacheParameters;
    uint4 renderParameters;
    uint4 reserved;
};

SharcParameters luminMakeSharcParameters(
    SharcGpuConstants constants,
    RWStructuredBuffer<uint64_t> hashEntries,
    RWStructuredBuffer<SharcAccumulationData> accumulation,
    RWStructuredBuffer<SharcPackedData> resolved,
    RWStructuredBuffer<uint> lockBuffer) {
    SharcParameters parameters;
    parameters.gridParameters.cameraPosition = constants.cameraPositionSceneScale.xyz;
    parameters.gridParameters.logarithmBase = constants.previousCameraPositionLogarithmBase.w;
    parameters.gridParameters.sceneScale = constants.cameraPositionSceneScale.w;
    parameters.gridParameters.levelBias = constants.traceParameters.z;
    parameters.hashMapData.capacity = constants.cacheParameters.x;
    parameters.hashMapData.hashEntriesBuffer = hashEntries;
    parameters.hashMapData.lockBuffer = lockBuffer;
    parameters.radianceScale = constants.toSunWorldRadianceScale.w;
    parameters.enableAntiFireflyFilter = constants.traceParameters.w > 0.5;
    parameters.accumulationBuffer = accumulation;
    parameters.resolvedBuffer = resolved;
    return parameters;
}

#endif

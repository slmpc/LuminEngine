#define LUMIN_SHARC_UPDATE 0
#include "SharcRuntime.slang"

static const uint kSharcStatisticOccupancy = 3;

[[vk::binding(0, 0)]] RWStructuredBuffer<uint64_t> sharcHashEntries;
[[vk::binding(1, 0)]] RWStructuredBuffer<SharcAccumulationData> sharcAccumulation;
[[vk::binding(2, 0)]] RWStructuredBuffer<SharcPackedData> sharcResolved;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> sharcLocks;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> sharcStatistics;
[[vk::binding(5, 0)]] ConstantBuffer<SharcGpuConstants> sharcFrame;

[shader("compute")]
[numthreads(256, 1, 1)]
void sharcResolveMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint entryIndex = dispatchThreadId.x;
    if (entryIndex >= sharcFrame.cacheParameters.x) {
        return;
    }

    SharcParameters parameters = luminMakeSharcParameters(
        sharcFrame, sharcHashEntries, sharcAccumulation, sharcResolved, sharcLocks);
    SharcResolveParameters resolveParameters;
    resolveParameters.cameraPositionPrev = sharcFrame.previousCameraPositionLogarithmBase.xyz;
    resolveParameters.accumulationFrameNum = sharcFrame.cacheParameters.y;
    resolveParameters.staleFrameNumMax = sharcFrame.cacheParameters.z;
    resolveParameters.frameIndex = sharcFrame.cacheParameters.w;
    SharcResolveEntry(entryIndex, parameters, resolveParameters);

    if (sharcHashEntries[entryIndex] != HASH_GRID_INVALID_HASH_KEY) {
        InterlockedAdd(sharcStatistics[kSharcStatisticOccupancy], 1);
    }
}

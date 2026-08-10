#include "AtmosphereCommon.slang"

[[vk::binding(0, 0)]] ConstantBuffer<AtmosphereGpuConstants> atmosphere;
[[vk::binding(4, 0)]] RWTexture2D<float4> transmittanceOutput;

[shader("compute")]
[numthreads(8, 8, 1)]
void transmittanceMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width;
    uint height;
    transmittanceOutput.GetDimensions(width, height);
    if (any(dispatchThreadId.xy >= uint2(width, height))) {
        return;
    }

    if (atmosphere.groundAlbedoAndEnabled.w < 0.5) {
        transmittanceOutput[dispatchThreadId.xy] = float4(1.0);
        return;
    }

    float2 unitCoordinate = float2(atmosphereUnitFromTexel(dispatchThreadId.x, width),
                                   atmosphereUnitFromTexel(dispatchThreadId.y, height));
    float radiusKm;
    float viewZenithCosine;
    atmosphereTransmittancePhysicalFromUnit(unitCoordinate, atmosphere, radiusKm, viewZenithCosine);

    float sinZenith = atmosphereSafeSqrt(1.0 - viewZenithCosine * viewZenithCosine);
    float3 originKm = float3(0.0, radiusKm, 0.0);
    float3 direction = float3(sinZenith, viewZenithCosine, 0.0);
    float2 topRoots = atmosphereRaySphereRoots(originKm, direction, atmosphere.atmosphereRadiiKm.y);
    float distanceToTopKm = max(topRoots.y, 0.0);
    float3 transmittance =
        atmosphereIntegrateTransmittance(originKm, direction, distanceToTopKm, 48, atmosphere);
    transmittanceOutput[dispatchThreadId.xy] = float4(transmittance, atmosphereAverage(transmittance));
}

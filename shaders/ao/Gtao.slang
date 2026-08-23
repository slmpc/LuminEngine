float computeGtao(float2 uv, float2 pixelPosition, float2 texelSize, float3 center, float3 normal,
                  float radius, float bias, float randomAngle) {
    const uint sliceCount = 6;
    const uint stepCount = 4;
    float pixelRadius = projectedRadiusPixels(center, radius);
    float3 viewDirection = normalize(frame.cameraPosition.xyz - center);
    float3 forward = normalize(frame.cameraForward.xyz);
    float3 referenceUp = abs(forward.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 cameraRight = normalize(cross(forward, referenceUp));
    float3 cameraUp = normalize(cross(cameraRight, forward));
    float visibilityLoss = 0.0;

    [unroll]
    for (uint sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
        float angle = randomAngle + 3.14159265 * float(sliceIndex) / float(sliceCount);
        float2 screenDirection = float2(cos(angle), sin(angle));
        float3 sliceTangent = normalize(cameraRight * screenDirection.x + cameraUp * screenDirection.y);
        float3 sliceNormal = normalize(cross(viewDirection, sliceTangent));
        float3 projectedNormal = normal - sliceNormal * dot(normal, sliceNormal);
        float projectedLength = length(projectedNormal);
        projectedNormal /= max(projectedLength, 1e-4);
        float2 horizons = 0.0;

        [unroll]
        for (uint stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
            float stepRadius =
                (float(stepIndex) - 0.5 + hash12(pixelPosition + float2(sliceIndex, stepIndex))) /
                float(stepCount);
            [unroll]
            for (uint side = 0; side < 2; ++side) {
                float signValue = side == 0 ? -1.0 : 1.0;
                float3 neighbor;
                if (!samplePosition(
                        uv + screenDirection * signValue * texelSize * pixelRadius * stepRadius, neighbor)) {
                    continue;
                }
                float3 delta = neighbor - center;
                float distanceToSample = length(delta);
                float elevation = max(dot(projectedNormal, delta / max(distanceToSample, 1e-4)) - bias, 0.0);
                horizons[side] = max(horizons[side], elevation * rangeFalloff(distanceToSample, radius));
            }
        }
        visibilityLoss += (horizons.x + horizons.y) * 0.5 * projectedLength;
    }
    return visibilityLoss * (1.2 / float(sliceCount));
}

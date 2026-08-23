float computeHbao(float2 uv, float2 pixelPosition, float2 texelSize, float3 center, float3 normal,
                  float radius, float bias, float randomAngle) {
    const uint directionCount = 8;
    const uint stepCount = 4;
    float pixelRadius = projectedRadiusPixels(center, radius);
    float occlusion = 0.0;

    [unroll]
    for (uint directionIndex = 0; directionIndex < directionCount; ++directionIndex) {
        float angle = randomAngle + 6.2831853 * float(directionIndex) / float(directionCount);
        float2 direction = float2(cos(angle), sin(angle));
        float horizon = 0.0;
        [unroll]
        for (uint stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
            float stepRadius = (float(stepIndex) - 0.35 +
                                hash12(pixelPosition + float2(stepIndex, directionIndex))) /
                               float(stepCount);
            float3 neighbor;
            if (!samplePosition(uv + direction * texelSize * pixelRadius * stepRadius, neighbor)) {
                continue;
            }
            float3 delta = neighbor - center;
            float distanceToSample = length(delta);
            float elevation = max(dot(normal, delta / max(distanceToSample, 1e-4)) - bias, 0.0);
            horizon = max(horizon, elevation * rangeFalloff(distanceToSample, radius));
        }
        occlusion += horizon;
    }
    return occlusion * (1.35 / float(directionCount));
}

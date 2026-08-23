float computeSsao(float2 uv, float2 texelSize, float3 center, float3 normal, float radius, float bias,
                  float randomAngle) {
    const uint sampleCount = 12;
    float2x2 rotation = float2x2(cos(randomAngle), -sin(randomAngle),
                                 sin(randomAngle), cos(randomAngle));
    float pixelRadius = projectedRadiusPixels(center, radius);
    float occlusion = 0.0;

    [unroll]
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float angle = (float(sampleIndex) + 0.5) * 2.3999632;
        float sampleRadius = sqrt((float(sampleIndex) + 1.0) / float(sampleCount));
        float2 direction = mul(rotation, float2(cos(angle), sin(angle)));
        float3 neighbor;
        if (!samplePosition(uv + direction * texelSize * pixelRadius * sampleRadius, neighbor)) {
            continue;
        }
        float3 delta = neighbor - center;
        float distanceToSample = length(delta);
        float horizon = max(dot(normal, delta / max(distanceToSample, 1e-4)) - bias, 0.0);
        occlusion += horizon * rangeFalloff(distanceToSample, radius);
    }
    return occlusion * (2.2 / float(sampleCount));
}

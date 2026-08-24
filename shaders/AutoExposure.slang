struct AutoExposurePushConstants {
    // xy: minimum/maximum EV; z: submitted-frame delta seconds; w: previous state valid.
    float4 exposureRange;
    // xy: brighten/darken adaptation speed; z: AgX metering curve enabled; w: reserved.
    float4 adaptation;
};

[[vk::push_constant]] ConstantBuffer<AutoExposurePushConstants> constants;
[[vk::binding(0, 0)]] Texture2D<float4> sceneTexture;
[[vk::binding(1, 0)]] Texture2D<float4> previousExposureTexture;
[[vk::binding(2, 0)]] SamplerState exposureSampler;

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    float2 triangleUv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(triangleUv.x * 2.0 - 1.0, 1.0 - triangleUv.y * 2.0, 0.0, 1.0);
    output.uv = triangleUv;
    return output;
}

float agxContrastApprox(float value) {
    value -= 20.0 / 33.0;
    const bool negative = value < 0.0;
    const float scale = negative ? 59.507875 : 69.862789;
    const float power0 = negative ? 3.0 : 13.0 / 4.0;
    const float power1 = negative ? -1.0 / 3.0 : -4.0 / 13.0;
    return 2.0 * value * pow(1.0 + scale * pow(abs(value), power0), power1) + 0.5;
}

float agxDisplayLuminance(float luminance) {
    const float minimumEv = -12.47393;
    const float maximumEv = 4.026069;
    const float logLuminance = clamp(log2(max(luminance, 1.0e-10)), minimumEv, maximumEv);
    return saturate(agxContrastApprox((logLuminance - minimumEv) / (maximumEv - minimumEv)));
}

float acesDisplayLuminance(float luminance) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((luminance * (a * luminance + b)) / (luminance * (c * luminance + d) + e));
}

float displayLuminance(float luminance) {
    return constants.adaptation.z >= 0.5 ? agxDisplayLuminance(luminance) : acesDisplayLuminance(luminance);
}

float sqrtTanhSoftLimit(float value, float strength) {
    return value * tanh(strength * sqrt(abs(value)));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    const bool historyValid = constants.exposureRange.w >= 0.5;
    const float4 previous = historyValid ? previousExposureTexture.Load(int3(0, 0, 0)) : float4(0.0);
    const float previousMultiplier = exp2(previous.z);

    const uint gridSize = 16;
    float averageLuminance = 0.0;
    float highlightWeight = 0.0;
    float shadowWeight = 0.0;
    float totalWeight = 0.0;
    for (uint y = 0; y < gridSize; ++y) {
        for (uint x = 0; x < gridSize; ++x) {
            const float2 uv = (float2(x, y) + 0.5) / float(gridSize);
            const float2 ndc = uv * 2.0 - 1.0;
            const float center = pow(saturate(1.0 - length(ndc)), 3.0);
            const float weight = 1.0 + center * 4.0;
            const float3 scene = max(sceneTexture.SampleLevel(exposureSampler, uv, 0.0).rgb, 0.0);
            const float sceneLuminance = dot(scene, float3(0.2126, 0.7152, 0.0722));
            const float measured = displayLuminance(sceneLuminance * previousMultiplier);
            averageLuminance += measured * weight;
            highlightWeight += (measured >= 225.0 / 255.0 ? weight : 0.0);
            shadowWeight += (measured <= 33.0 / 255.0 && sceneLuminance > 0.0 ? weight : 0.0);
            totalWeight += weight;
        }
    }
    averageLuminance /= max(totalWeight, 1.0e-6);

    const float minimumEv = constants.exposureRange.x;
    const float maximumEv = constants.exposureRange.y;
    const float normalizedExposure = saturate((previous.z - minimumEv) / max(maximumEv - minimumEv, 1.0e-5));
    const float targetCurve = pow(normalizedExposure * normalizedExposure, 0.25);
    const float luminanceTarget = lerp(140.0 / 255.0, 40.0 / 255.0, targetCurve);
    const float averageDelta = log2(luminanceTarget / max(averageLuminance, 1.0e-6));
    const float limitedAverageDelta = 4.0 / (1.0 + exp(-0.5 * averageDelta)) - 2.0;
    const float targetAverageEv = clamp(previous.x + limitedAverageDelta, minimumEv, maximumEv);

    const float desiredHighlightWeight = max(totalWeight * 0.05, 1.0e-6);
    const float desiredShadowWeight = max(totalWeight * 0.03, 1.0e-6);
    const float shadowDelta = log2(max(shadowWeight, 1.0e-6) / desiredShadowWeight);
    const float highlightDelta = log2(desiredHighlightWeight / max(highlightWeight, 1.0e-6));
    float protectionDelta = sqrtTanhSoftLimit(min(highlightDelta, 0.0), 0.15);
    protectionDelta = max(protectionDelta, sqrtTanhSoftLimit(max(shadowDelta, 0.0), 0.15));
    protectionDelta = clamp(protectionDelta, -1.5, 1.5);
    const float targetProtectionEv = clamp(previous.x + protectionDelta, minimumEv, maximumEv);

    const float targetEv = (targetAverageEv * 0.25 + targetProtectionEv) / 1.25;
    const float speed = targetEv >= previous.z ? constants.adaptation.x : constants.adaptation.y;
    const float deltaSeconds = clamp(constants.exposureRange.z, 1.0 / 240.0, 0.25);
    const float blend = historyValid ? 1.0 - exp(-max(speed, 1.0e-4) * deltaSeconds) : 1.0;
    const float nextAverageEv = lerp(previous.x, targetAverageEv, blend);
    const float nextProtectionEv = lerp(previous.y, targetProtectionEv, blend);
    const float nextEv = clamp((nextAverageEv * 0.25 + nextProtectionEv) / 1.25, minimumEv, maximumEv);
    return float4(nextAverageEv, nextProtectionEv, nextEv, protectionDelta);
}

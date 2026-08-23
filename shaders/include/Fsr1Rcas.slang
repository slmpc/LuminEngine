/*
 * This file is derived from AMD FidelityFX FSR1 RCAS.
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef LUMIN_FSR1_RCAS_SLANG
#define LUMIN_FSR1_RCAS_SLANG

static const float luminFsr1RcasLimit = 0.25 - (1.0 / 16.0);

float luminFsr1RcasLuma(float3 color) {
    return color.b * 0.5 + color.r * 0.5 + color.g;
}

float luminFsr1RcasMax3(float3 value) {
    return max(max(value.x, value.y), value.z);
}

float luminFsr1RcasSafeReciprocal(float value) {
    const float magnitude = max(abs(value), 1e-6);
    return (value < 0.0 ? -1.0 : 1.0) / magnitude;
}

/**
 * AMD FSR1 Robust Contrast Adaptive Sharpening 的 32-bit、带噪声抑制实现。
 * 输入必须是 `[0, 1]` 的线性显示颜色；`sharpness` 使用 Alpha-Piscium 的 `0..1` 用户参数语义。
 */
float3 luminFsr1Rcas(float3 north, float3 west, float3 center, float3 east, float3 south, float sharpness) {
    if (sharpness <= 0.0) {
        return center;
    }

    const float northLuma = luminFsr1RcasLuma(north);
    const float westLuma = luminFsr1RcasLuma(west);
    const float centerLuma = luminFsr1RcasLuma(center);
    const float eastLuma = luminFsr1RcasLuma(east);
    const float southLuma = luminFsr1RcasLuma(south);

    const float lumaMin = min(min(min(northLuma, westLuma), min(centerLuma, eastLuma)), southLuma);
    const float lumaMax = max(max(max(northLuma, westLuma), max(centerLuma, eastLuma)), southLuma);
    float noise = 0.25 * (northLuma + westLuma + eastLuma + southLuma) - centerLuma;
    noise = saturate(abs(noise) / max(lumaMax - lumaMin, 1e-6));
    noise = -0.5 * noise + 1.0;

    const float3 ringMin = min(min(north, west), min(east, south));
    const float3 ringMax = max(max(north, west), max(east, south));
    const float lowerLimiter = saturate(centerLuma / max(min(min(northLuma, westLuma), min(eastLuma, southLuma)),
                                                          1e-6));
    const float3 hitMin = ringMin / max(4.0 * ringMax, float3(1e-6)) * lowerLimiter;
    const float3 hitMax = (1.0 - ringMax) * float3(luminFsr1RcasSafeReciprocal(4.0 * ringMin.x - 4.0),
                                                   luminFsr1RcasSafeReciprocal(4.0 * ringMin.y - 4.0),
                                                   luminFsr1RcasSafeReciprocal(4.0 * ringMin.z - 4.0));
    const float3 lobeChannels = max(-hitMin, hitMax);

    // 参考 Alpha-Piscium：用户锐度 0..1 映射到 RCAS 2..0 stops，零值单独保持严格直通。
    const float sharpnessScale = exp2(-2.0 * (1.0 - saturate(sharpness)));
    float lobe = max(-luminFsr1RcasLimit, min(luminFsr1RcasMax3(lobeChannels), 0.0));
    lobe *= sharpnessScale * noise;

    return (lobe * (north + west + east + south) + center) / (4.0 * lobe + 1.0);
}

#endif

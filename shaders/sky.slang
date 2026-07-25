struct PostProcessUniforms {
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4x4 cascadeViewProjections[4];
    float4 cascadeSplits;
    float4 cameraPosition;
    float4 cameraForward;
    float4 lightDirection;
    float4 renderSize;
    float4 renderOptions;
    float4 tonemapOptions;
};

[[vk::binding(13, 0)]] ConstantBuffer<PostProcessUniforms> frame;

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

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 clipPosition = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    float4 worldFar = mul(frame.inverseViewProjection, float4(clipPosition, 1.0, 1.0));
    float3 rayDirection = normalize(worldFar.xyz / max(abs(worldFar.w), 1e-5) - frame.cameraPosition.xyz);
    float horizon = saturate(rayDirection.y * 0.5 + 0.5);
    float3 horizonColor = float3(0.36, 0.55, 0.78);
    float3 zenithColor = float3(0.055, 0.16, 0.36);
    float3 nadirColor = float3(0.025, 0.03, 0.04);
    float3 skyColor = rayDirection.y >= 0.0
        ? lerp(horizonColor, zenithColor, pow(horizon, 0.55))
        : lerp(horizonColor, nadirColor, saturate(-rayDirection.y * 3.0));

    float3 toSun = normalize(-frame.lightDirection.xyz);
    float sunAmount = saturate(dot(rayDirection, toSun));
    float sunDisk = pow(sunAmount, 1400.0) * 18.0;
    float sunGlow = pow(sunAmount, 24.0) * 0.8;
    skyColor += float3(1.0, 0.78, 0.48) * (sunDisk + sunGlow);
    return float4(skyColor, 1.0);
}

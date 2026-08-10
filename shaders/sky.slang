#include "PostProcessUniforms.slang"
#include "atmosphere/AtmosphereBindings.slang"

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
    float3 skyColor = atmosphereSampleEnvironmentRadiance(rayDirection);

    float3 toSun = normalize(atmosphere.toSunWorld.xyz);
    float sunAmount = saturate(dot(rayDirection, toSun));
    float sunDisk = pow(sunAmount, 1400.0) * 18.0;
    float sunGlow = pow(sunAmount, 24.0) * 0.8;
    float3 sunColor = max(atmosphere.sunColorIlluminanceLux.rgb, float3(0.0));
    skyColor += sunColor * (sunDisk + sunGlow);
    return float4(skyColor, 1.0);
}

[[vk::binding(0, 0)]] Texture2D<float4> positionTexture;
[[vk::binding(1, 0)]] Texture2D<float4> normalTexture;
[[vk::binding(2, 0)]] Texture2D<float4> albedoTexture;
[[vk::binding(3, 0)]] SamplerState gbufferSampler;

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(uv.x, 1.0 - uv.y);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 worldPosition = positionTexture.Sample(gbufferSampler, input.uv);
    if (worldPosition.w < 0.5) {
        return float4(0.035, 0.04, 0.05, 1.0);
    }

    float3 normal = normalize(normalTexture.Sample(gbufferSampler, input.uv).xyz);
    float3 albedo = albedoTexture.Sample(gbufferSampler, input.uv).rgb;
    float3 lightDirection = normalize(float3(0.45, 0.8, 0.35));
    float diffuse = max(dot(normal, lightDirection), 0.0);
    float3 hdrColor = albedo * (0.16 + diffuse * 1.35);

    // Reinhard 与 gamma 校正放在全屏 pass，证明 G-buffer 确实被后处理消费。
    float3 mapped = hdrColor / (hdrColor + 1.0);
    mapped = pow(mapped, 1.0 / 2.2);
    return float4(mapped, 1.0);
}

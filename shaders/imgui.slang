struct PushConstants {
    float2 scale;
    float2 translate;
    // x: target attachment performs sRGB encoding.
    float4 outputConfig;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> constants;
[[vk::binding(0, 0)]] Texture2D fontTexture;
[[vk::binding(128, 0)]] SamplerState fontSampler;

struct VertexInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position * constants.scale + constants.translate, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 fragmentMain(VertexOutput input) : SV_Target0 {
    float4 vertexColor = input.color;
    if (constants.outputConfig.x > 0.5) {
        vertexColor.rgb = select(vertexColor.rgb <= 0.04045, vertexColor.rgb / 12.92,
                                 pow((vertexColor.rgb + 0.055) / 1.055, 2.4));
    }
    return vertexColor * fontTexture.Sample(fontSampler, input.uv);
}

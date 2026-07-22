struct FrameUniforms {
    float4x4 viewProjection;
};

struct ObjectData {
    float4x4 model;
    float4 albedoRoughness;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame;

[[vk::binding(1, 0)]]
StructuredBuffer<ObjectData> objects;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    uint drawIndex : SV_DrawIndex;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal : TEXCOORD1;
    [[vk::location(2)]] float4 albedoRoughness : TEXCOORD2;
};

struct GBufferOutput {
    float4 position : SV_Target0;
    float4 normalRoughness : SV_Target1;
    float4 albedo : SV_Target2;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    ObjectData object = objects[input.drawIndex];
    float4 worldPosition = mul(object.model, float4(input.position, 1.0));

    VertexOutput output;
    output.position = mul(frame.viewProjection, worldPosition);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul((float3x3)object.model, input.normal));
    output.albedoRoughness = object.albedoRoughness;
    return output;
}

[shader("fragment")]
GBufferOutput fragmentMain(VertexOutput input) {
    GBufferOutput output;
    output.position = float4(input.worldPosition, 1.0);
    output.normalRoughness = float4(normalize(input.worldNormal), input.albedoRoughness.w);
    output.albedo = float4(input.albedoRoughness.rgb, 1.0);
    return output;
}

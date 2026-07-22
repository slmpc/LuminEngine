struct ShadowUniforms {
    float4x4 lightViewProjection;
};

struct ObjectData {
    float4x4 model;
    float4x4 previousModel;
    float4x4 normalMatrix;
    float4 baseColorMetallic;
    float4 materialParameters;
};

[[vk::binding(0, 0)]]
ConstantBuffer<ShadowUniforms> shadow;

[[vk::binding(1, 0)]]
StructuredBuffer<ObjectData> objects;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    uint drawIndex : SV_DrawIndex;
};

[shader("vertex")]
float4 vertexMain(VertexInput input) : SV_Position {
    float4 worldPosition = mul(objects[input.drawIndex].model, float4(input.position, 1.0));
    return mul(shadow.lightViewProjection, worldPosition);
}

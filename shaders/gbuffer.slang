struct FrameUniforms {
    float4x4 viewProjection;
    float4x4 previousViewProjection;
};

struct ObjectData {
    float4x4 model;
    float4x4 previousModel;
    float4x4 normalMatrix;
    float4 baseColorMetallic;
    float4 materialParameters;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> frame;

[[vk::binding(1, 0)]]
StructuredBuffer<ObjectData> objects;

[[vk::binding(2, 0)]]
Texture2DArray<float4> baseColorTextures;

[[vk::binding(3, 0)]]
Texture2DArray<float4> normalRoughnessTextures;

[[vk::binding(4, 0)]]
SamplerState materialSampler;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    uint drawIndex : SV_DrawIndex;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal : TEXCOORD1;
    [[vk::location(2)]] float2 texCoord : TEXCOORD2;
    [[vk::location(3)]] float4 baseColorMetallic : TEXCOORD3;
    [[vk::location(4)]] float4 materialParameters : TEXCOORD4;
    [[vk::location(5)]] float4 currentClip : TEXCOORD5;
    [[vk::location(6)]] float4 previousClip : TEXCOORD6;
};

struct GBufferOutput {
    float4 position : SV_Target0;
    float4 normalRoughness : SV_Target1;
    float4 albedoMetallic : SV_Target2;
    float2 motion : SV_Target3;
};

float2 clipToUv(float4 clipPosition) {
    float2 ndc = clipPosition.xy / max(clipPosition.w, 1e-5);
    return ndc * 0.5 + 0.5;
}

float3 applyNormalMap(float3 geometricNormal, float3 worldPosition, float2 uv, float3 tangentNormal) {
    float3 positionDx = ddx(worldPosition);
    float3 positionDy = ddy(worldPosition);
    float2 uvDx = ddx(uv);
    float2 uvDy = ddy(uv);
    float3 positionDyPerp = cross(positionDy, geometricNormal);
    float3 positionDxPerp = cross(geometricNormal, positionDx);
    float3 tangent = positionDyPerp * uvDx.x + positionDxPerp * uvDy.x;
    float3 bitangent = positionDyPerp * uvDx.y + positionDxPerp * uvDy.y;
    float maximumLength = max(dot(tangent, tangent), dot(bitangent, bitangent));
    if (maximumLength < 1e-8) {
        return geometricNormal;
    }
    float inverseLength = rsqrt(maximumLength);
    return normalize(tangent * (tangentNormal.x * inverseLength) +
                     bitangent * (tangentNormal.y * inverseLength) + geometricNormal * tangentNormal.z);
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    ObjectData object = objects[input.drawIndex];
    float4 worldPosition = mul(object.model, float4(input.position, 1.0));
    float4 previousWorldPosition = mul(object.previousModel, float4(input.position, 1.0));

    VertexOutput output;
    output.currentClip = mul(frame.viewProjection, worldPosition);
    output.previousClip = mul(frame.previousViewProjection, previousWorldPosition);
    output.position = output.currentClip;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul((float3x3)object.normalMatrix, input.normal));
    output.texCoord = input.texCoord;
    output.baseColorMetallic = object.baseColorMetallic;
    output.materialParameters = object.materialParameters;
    return output;
}

[shader("fragment")]
GBufferOutput fragmentMain(VertexOutput input) {
    uint textureLayer = uint(input.materialParameters.z + 0.5);
    float textureScale = max(abs(input.materialParameters.y), 1e-4);
    float2 materialUv = input.texCoord * textureScale;
    float3 textureCoordinate = float3(materialUv, float(textureLayer));
    float4 baseColorSample = baseColorTextures.Sample(materialSampler, textureCoordinate);
    float4 normalRoughnessSample = normalRoughnessTextures.Sample(materialSampler, textureCoordinate);

    float3 worldNormal = normalize(input.worldNormal);
    if (textureLayer != 0) {
        float3 tangentNormal = normalRoughnessSample.xyz * 2.0 - 1.0;
        tangentNormal.y *= input.materialParameters.w;
        worldNormal = applyNormalMap(worldNormal, input.worldPosition, materialUv, normalize(tangentNormal));
    }

    GBufferOutput output;
    output.position = float4(input.worldPosition, 1.0);
    output.normalRoughness =
        float4(worldNormal, clamp(normalRoughnessSample.a * input.materialParameters.x, 0.04, 1.0));
    output.albedoMetallic =
        float4(baseColorSample.rgb * input.baseColorMetallic.rgb, saturate(input.baseColorMetallic.a));
    float2 currentUv = clipToUv(input.currentClip);
    float2 previousUv = clipToUv(input.previousClip);
    output.motion = currentUv - previousUv;
    return output;
}

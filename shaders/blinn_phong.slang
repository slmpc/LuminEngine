struct FrameUniforms {
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float4 cameraPosition;
    float4 lightPosition;
    float4 lightColor;
    float4 materialColor;
    float4 materialParams;
};

[[vk::binding(0, 0)]]
ConstantBuffer<FrameUniforms> uniforms;

struct VertexInput {
    [[vk::location(0)]]
    float3 position : POSITION;

    [[vk::location(1)]]
    float3 normal : NORMAL;

    [[vk::location(2)]]
    float2 texCoord : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;

    [[vk::location(0)]]
    float3 worldPosition : TEXCOORD0;

    [[vk::location(1)]]
    float3 worldNormal : TEXCOORD1;

    [[vk::location(2)]]
    float2 texCoord : TEXCOORD2;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;

    float4 worldPosition = mul(uniforms.model, float4(input.position, 1.0));
    output.position = mul(uniforms.projection, mul(uniforms.view, worldPosition));
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul((float3x3)uniforms.model, input.normal));
    output.texCoord = input.texCoord;

    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float ambientStrength = uniforms.materialParams.x;
    float specularStrength = uniforms.materialParams.y;
    float shininess = uniforms.materialParams.z;
    bool smoothShading = uniforms.materialParams.w > 0.5;

    float3 smoothNormal = normalize(input.worldNormal);
    float3 flatNormal = normalize(cross(ddx(input.worldPosition), ddy(input.worldPosition)));
    flatNormal = dot(flatNormal, smoothNormal) < 0.0 ? -flatNormal : flatNormal;
    float3 normal = smoothShading ? smoothNormal : flatNormal;
    float3 lightDirection = normalize(uniforms.lightPosition.xyz - input.worldPosition);
    float3 viewDirection = normalize(uniforms.cameraPosition.xyz - input.worldPosition);
    float3 halfVector = normalize(lightDirection + viewDirection);

    float diffuse = max(dot(normal, lightDirection), 0.0);
    float specular = pow(max(dot(normal, halfVector), 0.0), shininess) * specularStrength;

    float3 ambientTerm = ambientStrength * uniforms.materialColor.rgb;
    float3 diffuseTerm = diffuse * uniforms.materialColor.rgb * uniforms.lightColor.rgb;
    float3 specularTerm = specular * uniforms.lightColor.rgb;
    float3 color = ambientTerm + diffuseTerm + specularTerm;

    return float4(color, 1.0);
}

struct GpuPackedVertex {
    float3 position;
    float positionPadding;
    float3 normal;
    float normalPadding;
};

struct GpuInstanceData {
    float4x4 model;
    float4x4 normalMatrix;
    uint4 metadata;
};

struct GpuMaterialData {
    float4 baseColorMetallic;
    float4 specularColorShininess;
    float4 surfaceParameters;
    uint4 metadata;
};

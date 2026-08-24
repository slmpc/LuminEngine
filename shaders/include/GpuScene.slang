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

static const uint kLuminLightDirectional = 0u;
static const uint kLuminLightPoint = 1u;
static const uint kLuminLightSpot = 2u;

struct GpuLightData {
    float4 positionRange;
    float4 directionCosOuter;
    float4 colorIntensity;
    float4 parameters;
};

uint luminLightType(GpuLightData light) {
    return uint(light.parameters.z + 0.5);
}

bool luminLightCastsShadows(GpuLightData light) {
    return light.parameters.w >= 0.5;
}

// GPU Scene 与 raster G-buffer 都按双面几何处理。命中背面时将插值法线翻向入射光线，避免负绕序或
// 镜像实例会把直接光与 SHARC cache 使用的法线污染为相反半球。
float3 luminOrientShadingNormal(float3 worldNormal, float3 rayDirection) {
    float lengthSquared = dot(worldNormal, worldNormal);
    float3 normal = lengthSquared > 1e-12 ? worldNormal * rsqrt(lengthSquared) : float3(0.0, 1.0, 0.0);
    return dot(normal, rayDirection) <= 0.0 ? normal : -normal;
}

float2 luminVertexTexCoord(GpuPackedVertex vertex) {
    return float2(vertex.positionPadding, vertex.normalPadding);
}

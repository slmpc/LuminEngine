#ifndef LUMIN_PHYSICAL_LIGHTING_SLANG
#define LUMIN_PHYSICAL_LIGHTING_SLANG

static const float kLuminPi = 3.14159265358979323846;
static const float kLuminFp16Max = 65504.0;
static const uint kLuminBlinnPhongSurfaceModel = 1u;

struct LuminCookTorranceMaterial {
    float3 baseColor;
    float3 diffuseColor;
    float3 reflectanceAtNormal;
    float roughness;
};

struct LuminCookTorranceLobes {
    float3 diffuse;
    float3 specular;
};

struct LuminBrdfSample {
    float3 direction;
    float3 throughput;
    float pdf;
    uint valid;
};

float3 luminPhysicalSafeNormalize(float3 value, float3 fallback) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12 ? value * rsqrt(lengthSquared) : fallback;
}

float3 luminClampFp16Radiance(float3 radiance) {
    return min(max(radiance, float3(0.0)), float3(kLuminFp16Max));
}

LuminCookTorranceMaterial luminMakeCookTorranceMaterial(GpuMaterialData material) {
    LuminCookTorranceMaterial result;
    result.baseColor = saturate(material.baseColorMetallic.rgb);
    result.roughness = clamp(material.surfaceParameters.x, 0.04, 1.0);
    if (material.metadata.x == kLuminBlinnPhongSurfaceModel) {
        // 旧材质的 specularColor 作为介质 F0；粗糙度仍由统一的 GGX 分布解释。
        result.diffuseColor = result.baseColor;
        result.reflectanceAtNormal = saturate(material.specularColorShininess.rgb);
    } else {
        float metallic = saturate(material.baseColorMetallic.a);
        result.diffuseColor = result.baseColor * (1.0 - metallic);
        result.reflectanceAtNormal = lerp(float3(0.04), result.baseColor, metallic);
    }
    return result;
}

float3 luminFresnelSchlick(float viewDotHalf, float3 reflectanceAtNormal) {
    float grazing = pow(1.0 - saturate(viewDotHalf), 5.0);
    return reflectanceAtNormal + (1.0 - reflectanceAtNormal) * grazing;
}

float luminGgxDistribution(float normalDotHalf, float roughness) {
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float denominator = normalDotHalf * normalDotHalf * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(kLuminPi * denominator * denominator, 1.0e-7);
}

float luminSmithGgxG1(float normalDotDirection, float roughness) {
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float cosineSquared = normalDotDirection * normalDotDirection;
    return (2.0 * normalDotDirection) /
           max(normalDotDirection + sqrt(alphaSquared + (1.0 - alphaSquared) * cosineSquared), 1.0e-6);
}

LuminCookTorranceLobes luminEvaluateCookTorrance(
    LuminCookTorranceMaterial material, float3 normal, float3 toView, float3 toLight) {
    LuminCookTorranceLobes result;
    result.diffuse = float3(0.0);
    result.specular = float3(0.0);

    float normalDotView = saturate(dot(normal, toView));
    float normalDotLight = saturate(dot(normal, toLight));
    if (normalDotView <= 0.0 || normalDotLight <= 0.0) {
        return result;
    }

    float3 halfVector = luminPhysicalSafeNormalize(toView + toLight, normal);
    float normalDotHalf = saturate(dot(normal, halfVector));
    float viewDotHalf = saturate(dot(toView, halfVector));
    float3 fresnel = luminFresnelSchlick(viewDotHalf, material.reflectanceAtNormal);
    float distribution = luminGgxDistribution(normalDotHalf, material.roughness);
    float geometry = luminSmithGgxG1(normalDotView, material.roughness) *
                     luminSmithGgxG1(normalDotLight, material.roughness);

    // Cook-Torrance: D * G * F / (4 NoV NoL)，漫反射按剩余菲涅耳能量守恒。
    result.specular = distribution * geometry * fresnel /
                      max(4.0 * normalDotView * normalDotLight, 1.0e-6);
    result.diffuse = (1.0 - fresnel) * material.diffuseColor / kLuminPi;
    return result;
}

float3 luminTangentToWorld(float3 localDirection, float3 normal) {
    float3 tangent = luminPhysicalSafeNormalize(
        abs(normal.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), normal)
                             : cross(float3(0.0, 1.0, 0.0), normal),
        float3(1.0, 0.0, 0.0));
    float3 bitangent = cross(normal, tangent);
    return luminPhysicalSafeNormalize(
        tangent * localDirection.x + bitangent * localDirection.y + normal * localDirection.z, normal);
}

LuminBrdfSample luminSampleCookTorranceDiffuse(
    LuminCookTorranceMaterial material, float3 normal, float3 toView, float2 random) {
    float radius = sqrt(saturate(random.x));
    float angle = 2.0 * kLuminPi * random.y;
    float3 localDirection = float3(radius * cos(angle), radius * sin(angle),
                                   sqrt(max(1.0 - random.x, 0.0)));

    LuminBrdfSample result;
    result.direction = luminTangentToWorld(localDirection, normal);
    float normalDotLight = saturate(dot(normal, result.direction));
    result.pdf = normalDotLight / kLuminPi;
    LuminCookTorranceLobes lobes = luminEvaluateCookTorrance(material, normal, toView, result.direction);
    result.valid = result.pdf > 1.0e-7 ? 1u : 0u;
    result.throughput = result.valid != 0u ? lobes.diffuse * (normalDotLight / result.pdf) : float3(0.0);
    return result;
}

LuminBrdfSample luminSampleCookTorranceSpecular(
    LuminCookTorranceMaterial material, float3 normal, float3 toView, float2 random) {
    float alpha = material.roughness * material.roughness;
    float alphaSquared = alpha * alpha;
    float phi = 2.0 * kLuminPi * random.x;
    float cosineTheta = sqrt((1.0 - random.y) / max(1.0 + (alphaSquared - 1.0) * random.y, 1.0e-7));
    float sineTheta = sqrt(max(1.0 - cosineTheta * cosineTheta, 0.0));
    float3 halfVector = luminTangentToWorld(
        float3(sineTheta * cos(phi), sineTheta * sin(phi), cosineTheta), normal);

    LuminBrdfSample result;
    result.direction = reflect(-toView, halfVector);
    float normalDotLight = saturate(dot(normal, result.direction));
    float normalDotHalf = saturate(dot(normal, halfVector));
    float viewDotHalf = saturate(dot(toView, halfVector));
    float distribution = luminGgxDistribution(normalDotHalf, material.roughness);
    result.pdf = distribution * normalDotHalf / max(4.0 * viewDotHalf, 1.0e-7);
    LuminCookTorranceLobes lobes = luminEvaluateCookTorrance(material, normal, toView, result.direction);
    result.valid = normalDotLight > 0.0 && result.pdf > 1.0e-7 ? 1u : 0u;
    result.throughput = result.valid != 0u ? lobes.specular * (normalDotLight / result.pdf) : float3(0.0);
    return result;
}

#endif

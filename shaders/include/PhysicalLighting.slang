#ifndef LUMIN_PHYSICAL_LIGHTING_SLANG
#define LUMIN_PHYSICAL_LIGHTING_SLANG

import GpuScene;

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
        // 旧 Ks 仅保留给 Raster Blinn-Phong；RT 使用由 MTL Ni/材质 IOR 推导并限制过的介质 F0。
        result.diffuseColor = result.baseColor;
        result.reflectanceAtNormal = float3(clamp(material.surfaceParameters.w, 0.0, 0.25));
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
    return alphaSquared / max(kLuminPi * denominator * denominator, 1.0e-12);
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

void luminBuildTangentFrame(float3 normal, out float3 tangent, out float3 bitangent) {
    tangent = luminPhysicalSafeNormalize(
        abs(normal.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), normal)
                             : cross(float3(0.0, 1.0, 0.0), normal),
        float3(1.0, 0.0, 0.0));
    bitangent = cross(normal, tangent);
}

float3 luminTangentToWorld(float3 localDirection, float3 normal) {
    float3 tangent;
    float3 bitangent;
    luminBuildTangentFrame(normal, tangent, bitangent);
    return luminPhysicalSafeNormalize(
        tangent * localDirection.x + bitangent * localDirection.y + normal * localDirection.z, normal);
}

float3 luminWorldToTangent(float3 worldDirection, float3 normal) {
    float3 tangent;
    float3 bitangent;
    luminBuildTangentFrame(normal, tangent, bitangent);
    return float3(dot(worldDirection, tangent), dot(worldDirection, bitangent), dot(worldDirection, normal));
}

float3 luminSampleGgxVisibleNormal(
    float3 normal, float3 toView, float roughness, float2 random) {
    float alpha = roughness * roughness;
    float3 viewLocal = luminWorldToTangent(toView, normal);
    float3 stretchedView = luminPhysicalSafeNormalize(
        float3(alpha * viewLocal.x, alpha * viewLocal.y, max(viewLocal.z, 1.0e-6)),
        float3(0.0, 0.0, 1.0));

    // Heitz GGX VNDF：先在拉伸空间采样可见微表面法线，再映射回原始椭球分布。
    float projectedLengthSquared = dot(stretchedView.xy, stretchedView.xy);
    float3 tangent1 = projectedLengthSquared > 1.0e-7
        ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(projectedLengthSquared)
        : float3(1.0, 0.0, 0.0);
    float3 tangent2 = cross(stretchedView, tangent1);
    float radius = sqrt(saturate(random.x));
    float phi = 2.0 * kLuminPi * random.y;
    float diskX = radius * cos(phi);
    float diskY = radius * sin(phi);
    float viewBlend = 0.5 * (1.0 + stretchedView.z);
    diskY = lerp(sqrt(saturate(1.0 - diskX * diskX)), diskY, viewBlend);
    float3 visibleNormal = diskX * tangent1 + diskY * tangent2 +
                           sqrt(saturate(1.0 - diskX * diskX - diskY * diskY)) * stretchedView;
    float3 halfLocal = luminPhysicalSafeNormalize(
        float3(alpha * visibleNormal.x, alpha * visibleNormal.y, max(visibleNormal.z, 0.0)),
        float3(0.0, 0.0, 1.0));
    return luminTangentToWorld(halfLocal, normal);
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
    LuminBrdfSample result;
    result.direction = normal;
    result.throughput = float3(0.0);
    result.pdf = 0.0;
    result.valid = 0u;
    float normalDotView = saturate(dot(normal, toView));
    if (normalDotView <= 1.0e-6) {
        return result;
    }

    float3 halfVector = luminSampleGgxVisibleNormal(normal, toView, material.roughness, random);
    result.direction = reflect(-toView, halfVector);
    float normalDotLight = saturate(dot(normal, result.direction));
    float normalDotHalf = saturate(dot(normal, halfVector));
    float viewDotHalf = saturate(dot(toView, halfVector));
    float distribution = luminGgxDistribution(normalDotHalf, material.roughness);
    float geometryView = luminSmithGgxG1(normalDotView, material.roughness);
    // VNDF half-vector PDF 经反射 Jacobian 转到出射方向后，VoH 项相消。
    result.pdf = distribution * geometryView / max(4.0 * normalDotView, 1.0e-7);
    LuminCookTorranceLobes lobes = luminEvaluateCookTorrance(material, normal, toView, result.direction);
    result.valid = normalDotLight > 0.0 && viewDotHalf > 0.0 && result.pdf > 1.0e-7 ? 1u : 0u;
    result.throughput = result.valid != 0u ? lobes.specular * (normalDotLight / result.pdf) : float3(0.0);
    return result;
}

#endif

#include "ShaderCatalog.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lumin::render {
    namespace {

        ShaderBindingDesc binding(std::string_view name, ShaderBindingKind kind, std::uint32_t set,
                                  std::uint32_t slot) {
            return ShaderBindingDesc{std::string(name), kind, set, slot};
        }

        ShaderBindingDesc pushConstant(std::string_view name, std::uint32_t slot) {
            return ShaderBindingDesc{std::string(name), ShaderBindingKind::PushConstant, std::nullopt, slot};
        }

        ShaderAbiFieldDesc field(std::string_view name, std::uint32_t offset, std::uint32_t size) {
            return ShaderAbiFieldDesc{std::string(name), offset, size};
        }

        void addRayTracingCapabilities(ShaderModuleBuilder& module) {
            module.capabilities({"spvRayTracingKHR", "SPV_KHR_non_semantic_info", "SPV_GOOGLE_user_type",
                                 "spvDerivativeControl", "spvImageQuery", "spvImageGatherExtended",
                                 "spvSparseResidency", "spvMinLod", "spvFragmentFullyCoveredEXT"});
        }

        void addAtmosphereConsumerBindings(ShaderModuleBuilder& module) {
            module.bindings({
                binding("atmosphere", ShaderBindingKind::ConstantBuffer, 2, 0),
                binding("atmosphereTransmittanceLut", ShaderBindingKind::SampledImage, 2, 1),
                binding("atmosphereMultiScatteringLut", ShaderBindingKind::SampledImage, 2, 2),
                binding("atmosphereSkyViewLut", ShaderBindingKind::SampledImage, 2, 3),
                binding("atmosphereAerialPerspectiveLut", ShaderBindingKind::SampledImage3D, 2, 4),
                binding("atmosphereSampler", ShaderBindingKind::Sampler, 2, 5),
            });
        }

        ShaderCatalog makeCatalog() {
            ShaderCatalogBuilder builder;
            builder
                .abiStruct("PostProcessUniforms", 544,
                           {field("inverseViewProjection", 0, 64), field("viewProjection", 64, 64),
                            field("cascadeViewProjections", 128, 256), field("cascadeSplits", 384, 16),
                            field("cameraPosition", 400, 16), field("cameraForward", 416, 16),
                            field("lightDirection", 432, 16), field("renderSize", 448, 16),
                            field("renderOptions", 464, 16), field("tonemapOptions", 480, 16),
                            field("ambientOcclusionOptions", 496, 16), field("temporalOptions", 512, 16),
                            field("autoExposureOptions", 528, 16)})
                .abiStruct("FrameUniforms", 128,
                           {field("viewProjection", 0, 64), field("previousViewProjection", 64, 64)})
                .abiStruct("ObjectData", 240,
                           {field("model", 0, 64), field("previousModel", 64, 64), field("normalMatrix", 128, 64),
                            field("baseColorMetallic", 192, 16), field("materialParameters", 208, 16),
                            field("metadata", 224, 16)})
                .abiStruct("ShadowUniforms", 64, {field("lightViewProjection", 0, 64)})
                .abiStruct("PushConstants", 32,
                           {field("scale", 0, 8), field("translate", 8, 8), field("outputConfig", 16, 16)})
                .abiStruct("BloomPushConstants", 32, {field("filter", 0, 16), field("controls", 16, 16)})
                .abiStruct("AutoExposurePushConstants", 32,
                           {field("exposureRange", 0, 16), field("adaptation", 16, 16)})
                .abiStruct("GpuPackedVertex", 32,
                           {field("position", 0, 12), field("positionPadding", 12, 4), field("normal", 16, 12),
                            field("normalPadding", 28, 4)})
                .abiStruct("GpuInstanceData", 144,
                           {field("model", 0, 64), field("normalMatrix", 64, 64), field("metadata", 128, 16)})
                .abiStruct("GpuMaterialData", 64,
                           {field("baseColorMetallic", 0, 16), field("specularColorShininess", 16, 16),
                            field("surfaceParameters", 32, 16), field("metadata", 48, 16)})
                .abiStruct("GpuLightData", 64,
                           {field("positionRange", 0, 16), field("directionCosOuter", 16, 16),
                            field("colorIntensity", 32, 16), field("parameters", 48, 16)})
                .abiStruct("SharcIndirectLightingConstants", 80,
                           {field("cameraPosition", 0, 16), field("cameraForward", 16, 16),
                            field("renderParameters", 32, 16), field("traceParameters", 48, 16),
                            field("samplingParameters", 64, 16)})
                .abiStruct("RayTracedDiConstants", 240,
                           {field("inverseViewProjection", 0, 64), field("previousViewProjection", 64, 64),
                            field("cameraPosition", 128, 16), field("cameraForward", 144, 16),
                            field("toSunWorld", 160, 16), field("sunIrradiance", 176, 16), field("renderSize", 192, 16),
                            field("traceParameters", 208, 16), field("samplingParameters", 224, 16)})
                .abiStruct(
                    "RtDiNrdInputsConstants", 48,
                    {field("cameraPosition", 0, 16), field("renderParameters", 16, 16), field("renderInfo", 32, 16)})
                .abiStruct("GiCompositeConstants", 48,
                           {field("cameraPosition", 0, 16), field("renderInfo", 16, 16), field("options", 32, 16)})
                .abiStruct("HybridLightingCompositeConstants", 16, {field("renderInfo", 0, 16)})
                .abiStruct("SharcGpuConstants", 128,
                           {field("cameraPositionSceneScale", 0, 16),
                            field("previousCameraPositionLogarithmBase", 16, 16),
                            field("toSunWorldRadianceScale", 32, 16), field("sunIrradiance", 48, 16),
                            field("traceParameters", 64, 16), field("cacheParameters", 80, 16),
                            field("renderParameters", 96, 16), field("samplingParameters", 112, 16)})
                .abiStruct("SharcAccumulationData", 16, {field("data", 0, 16)})
                .abiStruct("SharcPackedData", 16,
                           {field("radianceData", 0, 8), field("sampleData", 8, 4), field("sampleDataExt", 12, 4)})
                .abiStruct("AtmosphereGpuConstants", 528,
                           {field("view", 0, 64), field("projection", 64, 64), field("inverseView", 128, 64),
                            field("inverseProjection", 192, 64), field("inverseViewProjection", 256, 64),
                            field("cameraPositionWorld", 320, 16), field("cameraPlanetPositionKm", 336, 16),
                            field("toSunWorld", 352, 16), field("sunColorIlluminanceLux", 368, 16),
                            field("atmosphereRadiiKm", 384, 16), field("worldMappingAndClipKm", 400, 16),
                            field("renderExtent", 416, 16), field("rayleighScatteringAndInvScaleHeight", 432, 16),
                            field("mieScatteringAndInvScaleHeight", 448, 16), field("mieAbsorptionAndPhaseG", 464, 16),
                            field("ozoneAbsorptionPerKm", 480, 16), field("ozoneDensityProfileKm", 496, 16),
                            field("groundAlbedoAndEnabled", 512, 16)});

            builder.module("Deferred.slang")
                .bindings({binding("positionTexture", ShaderBindingKind::SampledImage, 0, 0),
                           binding("normalTexture", ShaderBindingKind::SampledImage, 0, 1),
                           binding("albedoTexture", ShaderBindingKind::SampledImage, 0, 2),
                           binding("globalIlluminationTexture", ShaderBindingKind::SampledImage, 0, 4),
                           binding("shadowTexture0", ShaderBindingKind::SampledImage, 0, 8),
                           binding("shadowTexture1", ShaderBindingKind::SampledImage, 0, 9),
                           binding("shadowTexture2", ShaderBindingKind::SampledImage, 0, 10),
                           binding("shadowTexture3", ShaderBindingKind::SampledImage, 0, 11),
                           binding("renderSampler", ShaderBindingKind::Sampler, 0, 12),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 13),
                           binding("materialIdTexture", ShaderBindingKind::SampledImage, 1, 0),
                           binding("materials", ShaderBindingKind::StructuredBuffer, 1, 1)})
                .abiStructs({"PostProcessUniforms", "GpuMaterialData"})
                .entry(ShaderId::DeferredVertex, "deferred.vertex", "vertexMain", ShaderStage::Vertex, "Deferred.vert")
                .entry(ShaderId::DeferredFragment, "deferred.fragment", "fragmentMain", ShaderStage::Fragment,
                       "Deferred.frag");

            builder.module("GBuffer.slang")
                .bindings({binding("frame", ShaderBindingKind::ConstantBuffer, 0, 0),
                           binding("objects", ShaderBindingKind::StructuredBuffer, 0, 1),
                           binding("baseColorTextures", ShaderBindingKind::SampledImage, 0, 2),
                           binding("normalRoughnessTextures", ShaderBindingKind::SampledImage, 0, 3),
                           binding("materialSampler", ShaderBindingKind::Sampler, 0, 4)})
                .abiStructs({"FrameUniforms", "ObjectData"})
                .entry(ShaderId::GBufferVertex, "gbuffer.vertex", "vertexMain", ShaderStage::Vertex, "GBuffer.vert")
                .entry(ShaderId::GBufferFragment, "gbuffer.fragment", "fragmentMain", ShaderStage::Fragment,
                       "GBuffer.frag",
                       {"SPV_KHR_non_semantic_info", "SPV_GOOGLE_user_type", "spvDerivativeControl", "spvImageQuery",
                        "spvImageGatherExtended", "spvSparseResidency", "spvMinLod", "spvFragmentFullyCoveredEXT"});

            builder.module("GiComposite.slang")
                .requireFeatures({ShaderFeature::RayTracing, ShaderFeature::Nrd})
                .includeDirectories({"../thirdparty/nrd/Shaders"})
                .bindings({binding("denoisedDiffuseRadianceHitDistance", ShaderBindingKind::SampledImage, 0, 0),
                           binding("denoisedSpecularRadianceHitDistance", ShaderBindingKind::SampledImage, 0, 1),
                           binding("positionTexture", ShaderBindingKind::SampledImage, 0, 2),
                           binding("normalRoughnessTexture", ShaderBindingKind::SampledImage, 0, 3),
                           binding("albedoMetallicTexture", ShaderBindingKind::SampledImage, 0, 4),
                           binding("materialIdTexture", ShaderBindingKind::SampledImage, 0, 5),
                           binding("materials", ShaderBindingKind::StructuredBuffer, 0, 6),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 7),
                           binding("lightingOutput", ShaderBindingKind::StorageImage, 0, 8),
                           binding("fallbackRadiance", ShaderBindingKind::SampledImage, 0, 9)})
                .abiStructs({"GpuMaterialData", "GiCompositeConstants"})
                .entry(ShaderId::GiCompositeCompute, "gi-composite.compute", "compositeMain", ShaderStage::Compute,
                       "GiComposite.comp");

            builder.module("HybridLightingComposite.slang")
                .requireFeatures({ShaderFeature::RayTracing})
                .bindings({binding("directRadiance", ShaderBindingKind::SampledImage, 0, 0),
                           binding("indirectRadiance", ShaderBindingKind::SampledImage, 0, 1),
                           binding("lightingOutput", ShaderBindingKind::StorageImage, 0, 2),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 3)})
                .abiStructs({"HybridLightingCompositeConstants"})
                .entry(ShaderId::HybridLightingCompositeCompute, "hybrid-lighting-composite.compute", "compositeMain",
                       ShaderStage::Compute, "HybridLightingComposite.comp");

            builder.module("ImGui.slang")
                .bindings({pushConstant("constants", 0), binding("fontTexture", ShaderBindingKind::SampledImage, 0, 0),
                           binding("fontSampler", ShaderBindingKind::Sampler, 0, 128)})
                .abiStructs({"PushConstants"})
                .entry(ShaderId::ImGuiVertex, "imgui.vertex", "vertexMain", ShaderStage::Vertex, "ImGui.vert")
                .entry(ShaderId::ImGuiFragment, "imgui.fragment", "fragmentMain", ShaderStage::Fragment, "ImGui.frag");

            builder.module("Bloom.slang")
                .bindings({pushConstant("constants", 0),
                           binding("sourceTexture", ShaderBindingKind::SampledImage, 0, 0),
                           binding("baseTexture", ShaderBindingKind::SampledImage, 0, 1),
                           binding("bloomSampler", ShaderBindingKind::Sampler, 0, 2)})
                .abiStructs({"BloomPushConstants"})
                .entry(ShaderId::BloomVertex, "bloom.vertex", "vertexMain", ShaderStage::Vertex, "Bloom.vert")
                .entry(ShaderId::BloomFragment, "bloom.fragment", "fragmentMain", ShaderStage::Fragment, "Bloom.frag");

            builder.module("AutoExposure.slang")
                .bindings({pushConstant("constants", 0), binding("sceneTexture", ShaderBindingKind::SampledImage, 0, 0),
                           binding("previousExposureTexture", ShaderBindingKind::SampledImage, 0, 1),
                           binding("exposureSampler", ShaderBindingKind::Sampler, 0, 2)})
                .abiStructs({"AutoExposurePushConstants"})
                .entry(ShaderId::AutoExposureVertex, "auto-exposure.vertex", "vertexMain", ShaderStage::Vertex,
                       "AutoExposure.vert")
                .entry(ShaderId::AutoExposureFragment, "auto-exposure.fragment", "fragmentMain", ShaderStage::Fragment,
                       "AutoExposure.frag");

            builder.module("PostProcess.slang")
                .bindings({binding("bloomTexture", ShaderBindingKind::SampledImage, 0, 14),
                           binding("autoExposureTexture", ShaderBindingKind::SampledImage, 0, 15),
                           binding("renderSampler", ShaderBindingKind::Sampler, 0, 12),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 13)})
                .abiStructs({"PostProcessUniforms"})
                .entry(ShaderId::PostProcessVertex, "postprocess.vertex", "vertexMain", ShaderStage::Vertex,
                       "PostProcess.vert")
                .entry(ShaderId::PostProcessFragment, "postprocess.fragment", "fragmentMain", ShaderStage::Fragment,
                       "PostProcess.frag");

            auto& rtDi = builder.module("RtDi.slang");
            rtDi.requireFeatures({ShaderFeature::RayTracing})
                .bindings({binding("sceneTlas", ShaderBindingKind::AccelerationStructure, 0, 0),
                           binding("vertexBuffers", ShaderBindingKind::StructuredBuffer, 0, 1),
                           binding("indexBuffers", ShaderBindingKind::StructuredBuffer, 0, 2),
                           binding("instances", ShaderBindingKind::StructuredBuffer, 0, 3),
                           binding("materials", ShaderBindingKind::StructuredBuffer, 0, 4),
                           binding("worldPositionHitT", ShaderBindingKind::StorageImage, 0, 5),
                           binding("normalRoughness", ShaderBindingKind::StorageImage, 0, 6),
                           binding("albedoMetallic", ShaderBindingKind::StorageImage, 0, 7),
                           binding("materialIdOutput", ShaderBindingKind::StorageImage, 0, 8),
                           binding("viewZ", ShaderBindingKind::StorageImage, 0, 9),
                           binding("motion", ShaderBindingKind::StorageImage, 0, 10),
                           binding("directRadiance", ShaderBindingKind::StorageImage, 0, 11),
                           binding("visibilityMask", ShaderBindingKind::StorageImage, 0, 12),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 13)})
                .abiStructs({"GpuPackedVertex", "GpuInstanceData", "GpuMaterialData", "GpuLightData",
                             "RayTracedDiConstants", "AtmosphereGpuConstants"});
            addRayTracingCapabilities(rtDi);
            addAtmosphereConsumerBindings(rtDi);
            rtDi.bindings({binding("baseColorTextures", ShaderBindingKind::SampledImage, 0, 14),
                           binding("normalRoughnessTextures", ShaderBindingKind::SampledImage, 0, 15),
                           binding("materialSampler", ShaderBindingKind::Sampler, 0, 16),
                           binding("lights", ShaderBindingKind::StructuredBuffer, 0, 17),
                           binding("directDiffuseRadianceHitT", ShaderBindingKind::StorageImage, 0, 18),
                           binding("directSpecularRadianceHitT", ShaderBindingKind::StorageImage, 0, 19)})
                .entry(ShaderId::RtDiRayGeneration, "rt-di.ray-generation", "rayGenerationMain",
                       ShaderStage::RayGeneration, "RtDi.rgen")
                .entry(ShaderId::RtDiRadianceMiss, "rt-di.radiance-miss", "primaryMissMain", ShaderStage::Miss,
                       "RtDi.radiance.rmiss")
                .entry(ShaderId::RtDiShadowMiss, "rt-di.shadow-miss", "shadowMissMain", ShaderStage::Miss,
                       "RtDi.shadow.rmiss")
                .entry(ShaderId::RtDiClosestHit, "rt-di.closest-hit", "primaryClosestHitMain", ShaderStage::ClosestHit,
                       "RtDi.rchit");

            builder.module("RtDiNrdInputs.slang")
                .requireFeatures({ShaderFeature::RayTracing, ShaderFeature::Nrd})
                .includeDirectories({"../thirdparty/nrd/Shaders"})
                .bindings({binding("rawDiffuseRadianceHitT", ShaderBindingKind::SampledImage, 0, 0),
                           binding("rawSpecularRadianceHitT", ShaderBindingKind::SampledImage, 0, 1),
                           binding("positionTexture", ShaderBindingKind::SampledImage, 0, 2),
                           binding("normalRoughnessTexture", ShaderBindingKind::SampledImage, 0, 3),
                           binding("albedoMetallicTexture", ShaderBindingKind::SampledImage, 0, 4),
                           binding("materialIdTexture", ShaderBindingKind::SampledImage, 0, 5),
                           binding("viewZTexture", ShaderBindingKind::SampledImage, 0, 6),
                           binding("motionTexture", ShaderBindingKind::SampledImage, 0, 7),
                           binding("materials", ShaderBindingKind::StructuredBuffer, 0, 8),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 9),
                           binding("diffuseRadianceHitDistance", ShaderBindingKind::StorageImage, 0, 10),
                           binding("specularRadianceHitDistance", ShaderBindingKind::StorageImage, 0, 11),
                           binding("denoiserViewZ", ShaderBindingKind::StorageImage, 0, 12),
                           binding("denoiserNormalRoughness", ShaderBindingKind::StorageImage, 0, 13),
                           binding("denoiserMotion", ShaderBindingKind::StorageImage, 0, 14)})
                .abiStructs({"GpuMaterialData", "RtDiNrdInputsConstants"})
                .entry(ShaderId::RtDiNrdInputsCompute, "rt-di-nrd-inputs.compute", "prepareMain", ShaderStage::Compute,
                       "RtDiNrdInputs.comp");

            auto& sharcIndirect = builder.module("SharcIndirectLighting.slang");
            sharcIndirect.requireFeatures({ShaderFeature::RayTracing, ShaderFeature::Nrd, ShaderFeature::Sharc})
                .includeDirectories({"../thirdparty/nrd/Shaders", "../thirdparty/sharc/include"})
                .bindings({binding("sceneTlas", ShaderBindingKind::AccelerationStructure, 0, 0),
                           binding("positionTexture", ShaderBindingKind::SampledImage, 0, 1),
                           binding("normalRoughnessTexture", ShaderBindingKind::SampledImage, 0, 2),
                           binding("albedoMetallicTexture", ShaderBindingKind::SampledImage, 0, 3),
                           binding("motionTexture", ShaderBindingKind::SampledImage, 0, 4),
                           binding("materialIdTexture", ShaderBindingKind::SampledImage, 0, 5),
                           binding("vertexBuffers", ShaderBindingKind::StructuredBuffer, 0, 6),
                           binding("indexBuffers", ShaderBindingKind::StructuredBuffer, 0, 7),
                           binding("instances", ShaderBindingKind::StructuredBuffer, 0, 8),
                           binding("materials", ShaderBindingKind::StructuredBuffer, 0, 9),
                           binding("diffuseRadianceHitDistance", ShaderBindingKind::StorageImage, 0, 10),
                           binding("specularRadianceHitDistance", ShaderBindingKind::StorageImage, 0, 11),
                           binding("viewZ", ShaderBindingKind::StorageImage, 0, 12),
                           binding("denoiserNormalRoughness", ShaderBindingKind::StorageImage, 0, 13),
                           binding("denoiserMotion", ShaderBindingKind::StorageImage, 0, 14),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 15),
                           binding("sharcHashEntries", ShaderBindingKind::StructuredBuffer, 0, 16),
                           binding("sharcAccumulation", ShaderBindingKind::StructuredBuffer, 0, 17),
                           binding("sharcResolved", ShaderBindingKind::StructuredBuffer, 0, 18),
                           binding("sharcLocks", ShaderBindingKind::StructuredBuffer, 0, 19),
                           binding("sharcStatistics", ShaderBindingKind::StructuredBuffer, 0, 20),
                           binding("sharcFrame", ShaderBindingKind::ConstantBuffer, 0, 21),
                           binding("baseColorTextures", ShaderBindingKind::SampledImage, 0, 22),
                           binding("normalRoughnessTextures", ShaderBindingKind::SampledImage, 0, 23),
                           binding("materialSampler", ShaderBindingKind::Sampler, 0, 24),
                           binding("lights", ShaderBindingKind::StructuredBuffer, 0, 25)})
                .abiStructs({"GpuPackedVertex", "GpuInstanceData", "GpuMaterialData", "GpuLightData",
                             "SharcIndirectLightingConstants", "AtmosphereGpuConstants", "SharcGpuConstants",
                             "SharcAccumulationData", "SharcPackedData"});
            addRayTracingCapabilities(sharcIndirect);
            addAtmosphereConsumerBindings(sharcIndirect);
            sharcIndirect
                .entry(ShaderId::SharcIndirectLightingRayGeneration, "sharc-indirect-lighting.ray-generation",
                       "sharcIndirectLightingRayGenerationMain", ShaderStage::RayGeneration,
                       "SharcIndirectLighting.rgen")
                .entry(ShaderId::SharcIndirectLightingRadianceMiss, "sharc-indirect-lighting.radiance-miss",
                       "sharcIndirectLightingRadianceMissMain", ShaderStage::Miss,
                       "SharcIndirectLighting.radiance.rmiss")
                .entry(ShaderId::SharcIndirectLightingShadowMiss, "sharc-indirect-lighting.shadow-miss",
                       "sharcIndirectLightingShadowMissMain", ShaderStage::Miss, "SharcIndirectLighting.shadow.rmiss")
                .entry(ShaderId::SharcIndirectLightingClosestHit, "sharc-indirect-lighting.closest-hit",
                       "sharcIndirectLightingClosestHitMain", ShaderStage::ClosestHit, "SharcIndirectLighting.rchit");

            builder.module("Shadow.slang")
                .bindings({binding("shadow", ShaderBindingKind::ConstantBuffer, 0, 0),
                           binding("objects", ShaderBindingKind::StructuredBuffer, 0, 1)})
                .abiStructs({"ShadowUniforms", "ObjectData"})
                .entry(ShaderId::ShadowVertex, "shadow.vertex", "vertexMain", ShaderStage::Vertex, "Shadow.vert");

            builder.module("SharcResolve.slang")
                .requireFeatures({ShaderFeature::RayTracing, ShaderFeature::Sharc})
                .includeDirectories({"../thirdparty/sharc/include"})
                .bindings({binding("sharcHashEntries", ShaderBindingKind::StructuredBuffer, 0, 0),
                           binding("sharcAccumulation", ShaderBindingKind::StructuredBuffer, 0, 1),
                           binding("sharcResolved", ShaderBindingKind::StructuredBuffer, 0, 2),
                           binding("sharcLocks", ShaderBindingKind::StructuredBuffer, 0, 3),
                           binding("sharcStatistics", ShaderBindingKind::StructuredBuffer, 0, 4),
                           binding("sharcFrame", ShaderBindingKind::ConstantBuffer, 0, 5)})
                .abiStructs({"SharcGpuConstants", "SharcAccumulationData", "SharcPackedData"})
                .entry(ShaderId::SharcResolveCompute, "sharc-resolve.compute", "sharcResolveMain", ShaderStage::Compute,
                       "SharcResolve.comp");

            auto& sharcUpdate = builder.module("SharcUpdate.slang");
            sharcUpdate.requireFeatures({ShaderFeature::RayTracing, ShaderFeature::Sharc})
                .includeDirectories({"../thirdparty/sharc/include"})
                .bindings({binding("sceneTlas", ShaderBindingKind::AccelerationStructure, 0, 0),
                           binding("positionTexture", ShaderBindingKind::SampledImage, 0, 1),
                           binding("normalRoughnessTexture", ShaderBindingKind::SampledImage, 0, 2),
                           binding("albedoMetallicTexture", ShaderBindingKind::SampledImage, 0, 3),
                           binding("vertexBuffers", ShaderBindingKind::StructuredBuffer, 0, 4),
                           binding("indexBuffers", ShaderBindingKind::StructuredBuffer, 0, 5),
                           binding("instances", ShaderBindingKind::StructuredBuffer, 0, 6),
                           binding("materials", ShaderBindingKind::StructuredBuffer, 0, 7),
                           binding("sharcHashEntries", ShaderBindingKind::StructuredBuffer, 0, 8),
                           binding("sharcAccumulation", ShaderBindingKind::StructuredBuffer, 0, 9),
                           binding("sharcResolved", ShaderBindingKind::StructuredBuffer, 0, 10),
                           binding("sharcLocks", ShaderBindingKind::StructuredBuffer, 0, 11),
                           binding("sharcStatistics", ShaderBindingKind::StructuredBuffer, 0, 12),
                           binding("sharcFrame", ShaderBindingKind::ConstantBuffer, 0, 13)})
                .abiStructs({"GpuPackedVertex", "GpuInstanceData", "GpuMaterialData", "AtmosphereGpuConstants",
                             "SharcGpuConstants", "SharcAccumulationData", "SharcPackedData"});
            addRayTracingCapabilities(sharcUpdate);
            addAtmosphereConsumerBindings(sharcUpdate);
            sharcUpdate
                .bindings({binding("baseColorTextures", ShaderBindingKind::SampledImage, 0, 14),
                           binding("normalRoughnessTextures", ShaderBindingKind::SampledImage, 0, 15),
                           binding("materialSampler", ShaderBindingKind::Sampler, 0, 16),
                           binding("materialIdTexture", ShaderBindingKind::SampledImage, 0, 17),
                           binding("lights", ShaderBindingKind::StructuredBuffer, 0, 18)})
                .abiStructs({"GpuLightData"})
                .entry(ShaderId::SharcUpdateRayGeneration, "sharc-update.ray-generation",
                       "sharcUpdateRayGenerationMain", ShaderStage::RayGeneration, "SharcUpdate.rgen")
                .entry(ShaderId::SharcUpdateRadianceMiss, "sharc-update.radiance-miss", "sharcUpdateRadianceMissMain",
                       ShaderStage::Miss, "SharcUpdate.radiance.rmiss")
                .entry(ShaderId::SharcUpdateShadowMiss, "sharc-update.shadow-miss", "sharcUpdateShadowMissMain",
                       ShaderStage::Miss, "SharcUpdate.shadow.rmiss")
                .entry(ShaderId::SharcUpdateClosestHit, "sharc-update.closest-hit", "sharcUpdateClosestHitMain",
                       ShaderStage::ClosestHit, "SharcUpdate.rchit");

            auto& sky = builder.module("Sky.slang");
            addAtmosphereConsumerBindings(sky);
            sky.bindings({binding("frame", ShaderBindingKind::ConstantBuffer, 0, 13)})
                .abiStructs({"PostProcessUniforms", "AtmosphereGpuConstants"})
                .entry(ShaderId::SkyVertex, "sky.vertex", "vertexMain", ShaderStage::Vertex, "Sky.vert")
                .entry(ShaderId::SkyFragment, "sky.fragment", "fragmentMain", ShaderStage::Fragment, "Sky.frag");

            builder.module("Taa.slang")
                .bindings({binding("positionTexture", ShaderBindingKind::SampledImage, 0, 0),
                           binding("motionTexture", ShaderBindingKind::SampledImage, 0, 3),
                           binding("lightingTexture", ShaderBindingKind::SampledImage, 0, 5),
                           binding("historyTexture", ShaderBindingKind::SampledImage, 0, 6),
                           binding("renderSampler", ShaderBindingKind::Sampler, 0, 12),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 13)})
                .abiStructs({"PostProcessUniforms"})
                .entry(ShaderId::TaaVertex, "taa.vertex", "vertexMain", ShaderStage::Vertex, "Taa.vert")
                .entry(ShaderId::TaaFragment, "taa.fragment", "fragmentMain", ShaderStage::Fragment, "Taa.frag");

            builder.module("ao/AmbientOcclusion.slang")
                .bindings({binding("positionTexture", ShaderBindingKind::SampledImage, 0, 0),
                           binding("normalTexture", ShaderBindingKind::SampledImage, 0, 1),
                           binding("renderSampler", ShaderBindingKind::Sampler, 0, 2),
                           binding("frame", ShaderBindingKind::ConstantBuffer, 0, 3)})
                .abiStructs({"PostProcessUniforms"})
                .entry(ShaderId::AmbientOcclusionVertex, "ambientOcclusion.vertex", "vertexMain", ShaderStage::Vertex,
                       "AmbientOcclusion.vert")
                .entry(ShaderId::AmbientOcclusionFragment, "ambientOcclusion.fragment", "fragmentMain",
                       ShaderStage::Fragment, "AmbientOcclusion.frag");

            builder.module("atmosphere/AerialPerspectiveLut.slang")
                .bindings({binding("atmosphere", ShaderBindingKind::ConstantBuffer, 0, 0),
                           binding("transmittanceLut", ShaderBindingKind::SampledImage, 0, 1),
                           binding("multiScatteringLut", ShaderBindingKind::SampledImage, 0, 2),
                           binding("atmosphereSampler", ShaderBindingKind::Sampler, 0, 3),
                           binding("aerialPerspectiveOutput", ShaderBindingKind::StorageImage3D, 0, 4)})
                .abiStructs({"AtmosphereGpuConstants"})
                .entry(ShaderId::AtmosphereAerialPerspective, "atmosphere.aerial-perspective", "aerialPerspectiveMain",
                       ShaderStage::Compute, "AerialPerspectiveLut.comp");

            builder.module("atmosphere/MultiScatteringLut.slang")
                .bindings({binding("atmosphere", ShaderBindingKind::ConstantBuffer, 0, 0),
                           binding("transmittanceLut", ShaderBindingKind::SampledImage, 0, 1),
                           binding("atmosphereSampler", ShaderBindingKind::Sampler, 0, 3),
                           binding("multiScatteringOutput", ShaderBindingKind::StorageImage, 0, 4)})
                .abiStructs({"AtmosphereGpuConstants"})
                .entry(ShaderId::AtmosphereMultiScattering, "atmosphere.multi-scattering", "multiScatteringMain",
                       ShaderStage::Compute, "MultiScatteringLut.comp");

            builder.module("atmosphere/SkyViewLut.slang")
                .bindings({binding("atmosphere", ShaderBindingKind::ConstantBuffer, 0, 0),
                           binding("transmittanceLut", ShaderBindingKind::SampledImage, 0, 1),
                           binding("multiScatteringLut", ShaderBindingKind::SampledImage, 0, 2),
                           binding("atmosphereSampler", ShaderBindingKind::Sampler, 0, 3),
                           binding("skyViewOutput", ShaderBindingKind::StorageImage, 0, 4)})
                .abiStructs({"AtmosphereGpuConstants"})
                .entry(ShaderId::AtmosphereSkyView, "atmosphere.sky-view", "skyViewMain", ShaderStage::Compute,
                       "SkyViewLut.comp");

            builder.module("atmosphere/TransmittanceLut.slang")
                .bindings({binding("atmosphere", ShaderBindingKind::ConstantBuffer, 0, 0),
                           binding("transmittanceOutput", ShaderBindingKind::StorageImage, 0, 4)})
                .abiStructs({"AtmosphereGpuConstants"})
                .entry(ShaderId::AtmosphereTransmittance, "atmosphere.transmittance", "transmittanceMain",
                       ShaderStage::Compute, "TransmittanceLut.comp");

            return std::move(builder).build();
        }

    } // namespace

    const ShaderEntryDesc& ShaderCatalog::entry(ShaderId id) const {
        const auto iterator = std::ranges::find(entries, id, &ShaderEntryDesc::id);
        if (iterator == entries.end()) {
            throw std::out_of_range("ShaderId is not registered in the shader catalog.");
        }
        return *iterator;
    }

    ShaderModuleBuilder::ShaderModuleBuilder(std::string source) : source_(std::move(source)) {
    }

    ShaderModuleBuilder& ShaderModuleBuilder::requireFeatures(std::initializer_list<ShaderFeature> features) {
        requirements_.insert(requirements_.end(), features);
        return *this;
    }

    ShaderModuleBuilder& ShaderModuleBuilder::capabilities(std::initializer_list<std::string_view> capabilities) {
        for (const std::string_view capability : capabilities) {
            capabilities_.emplace_back(capability);
        }
        return *this;
    }

    ShaderModuleBuilder& ShaderModuleBuilder::includeDirectories(std::initializer_list<std::string_view> directories) {
        for (const std::string_view directory : directories) {
            includeDirectories_.emplace_back(directory);
        }
        return *this;
    }

    ShaderModuleBuilder& ShaderModuleBuilder::bindings(std::initializer_list<ShaderBindingDesc> bindings) {
        bindings_.insert(bindings_.end(), bindings);
        return *this;
    }

    ShaderModuleBuilder& ShaderModuleBuilder::abiStructs(std::initializer_list<std::string_view> names) {
        for (const std::string_view name : names) {
            abiStructs_.emplace_back(name);
        }
        return *this;
    }

    ShaderModuleBuilder& ShaderModuleBuilder::entry(ShaderId id, std::string_view name, std::string_view entryPoint,
                                                    ShaderStage stage, std::string_view artifactStem,
                                                    std::initializer_list<std::string_view> extraCapabilities) {
        ShaderEntryDesc desc;
        desc.id = id;
        desc.name = name;
        desc.entryPoint = entryPoint;
        desc.stage = stage;
        desc.output = std::string(artifactStem) + ".spv";
        desc.reflection = "reflection/" + std::string(artifactStem) + ".json";
        desc.depfile = "deps/" + std::string(artifactStem) + ".d";
        for (const std::string_view capability : extraCapabilities) {
            desc.capabilities.emplace_back(capability);
        }
        entries_.push_back(std::move(desc));
        return *this;
    }

    ShaderCatalogBuilder& ShaderCatalogBuilder::abiStruct(std::string_view name, std::uint32_t size,
                                                          std::initializer_list<ShaderAbiFieldDesc> fields) {
        catalog_.abiStructs.push_back(ShaderAbiStructDesc{std::string(name), size, fields});
        return *this;
    }

    ShaderModuleBuilder& ShaderCatalogBuilder::module(std::string_view source) {
        modules_.push_back(ShaderModuleBuilder(std::string(source)));
        return modules_.back();
    }

    ShaderCatalog ShaderCatalogBuilder::build() && {
        std::unordered_set<std::string> abiNames;
        for (const ShaderAbiStructDesc& abi : catalog_.abiStructs) {
            if (abi.name.empty() || abi.size == 0 || abi.fields.empty() || !abiNames.insert(abi.name).second) {
                throw std::logic_error("Shader ABI structs require unique non-empty names, fields, and sizes.");
            }
        }

        const auto idCount = static_cast<std::size_t>(ShaderId::Count);
        std::vector<bool> ids(idCount, false);
        std::unordered_set<std::string> names;
        std::unordered_set<std::string> outputs;
        for (ShaderModuleBuilder& module : modules_) {
            if (module.source_.empty() || module.entries_.empty()) {
                throw std::logic_error("Shader modules require a source and at least one entry.");
            }
            for (ShaderEntryDesc& entry : module.entries_) {
                const auto id = static_cast<std::size_t>(entry.id);
                if (id >= idCount || ids[id]) {
                    throw std::logic_error("Shader catalog contains an invalid or duplicated ShaderId.");
                }
                if (entry.name.empty() || entry.entryPoint.empty() || !names.insert(entry.name).second ||
                    !outputs.insert(entry.output).second) {
                    throw std::logic_error("Shader catalog entry names and outputs must be non-empty and unique.");
                }
                ids[id] = true;
                entry.source = module.source_;
                entry.requirements = module.requirements_;
                entry.capabilities.insert(entry.capabilities.begin(), module.capabilities_.begin(),
                                          module.capabilities_.end());
                entry.includeDirectories = module.includeDirectories_;
                entry.bindings = module.bindings_;
                entry.abiStructs = module.abiStructs_;
                for (const std::string& abiName : entry.abiStructs) {
                    if (!abiNames.contains(abiName)) {
                        throw std::logic_error("Shader entry references an unknown ABI struct: " + abiName);
                    }
                }
                catalog_.entries.push_back(std::move(entry));
            }
        }
        if (std::ranges::find(ids, false) != ids.end()) {
            throw std::logic_error("Every ShaderId must be registered exactly once.");
        }
        return std::move(catalog_);
    }

    const ShaderCatalog& builtinShaderCatalog() {
        static const ShaderCatalog catalog = makeCatalog();
        return catalog;
    }

    std::string_view toString(ShaderStage stage) noexcept {
        switch (stage) {
        case ShaderStage::Compute:
            return "compute";
        case ShaderStage::Vertex:
            return "vertex";
        case ShaderStage::Fragment:
            return "fragment";
        case ShaderStage::RayGeneration:
            return "raygeneration";
        case ShaderStage::Miss:
            return "miss";
        case ShaderStage::ClosestHit:
            return "closesthit";
        }
        return "unknown";
    }

    std::string_view toString(ShaderFeature feature) noexcept {
        switch (feature) {
        case ShaderFeature::RayTracing:
            return "rayTracing";
        case ShaderFeature::Nrd:
            return "nrd";
        case ShaderFeature::Sharc:
            return "sharc";
        }
        return "unknown";
    }

    std::string_view toString(ShaderBindingKind kind) noexcept {
        switch (kind) {
        case ShaderBindingKind::ConstantBuffer:
            return "constantBuffer";
        case ShaderBindingKind::StructuredBuffer:
            return "structuredBuffer";
        case ShaderBindingKind::SampledImage:
            return "sampledImage";
        case ShaderBindingKind::SampledImage3D:
            return "sampledImage3D";
        case ShaderBindingKind::Sampler:
            return "sampler";
        case ShaderBindingKind::StorageImage:
            return "storageImage";
        case ShaderBindingKind::StorageImage3D:
            return "storageImage3D";
        case ShaderBindingKind::AccelerationStructure:
            return "accelerationStructure";
        case ShaderBindingKind::PushConstant:
            return "pushConstant";
        }
        return "unknown";
    }

} // namespace lumin::render

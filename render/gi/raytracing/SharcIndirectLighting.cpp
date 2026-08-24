#include "render/gi/raytracing/SharcIndirectLighting.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lumin::render::gi {
    namespace {

        constexpr std::uint32_t tlasBinding = 0;
        constexpr std::uint32_t positionBinding = 1;
        constexpr std::uint32_t normalRoughnessInputBinding = 2;
        constexpr std::uint32_t albedoMetallicBinding = 3;
        constexpr std::uint32_t motionInputBinding = 4;
        constexpr std::uint32_t materialIdInputBinding = 5;
        constexpr std::uint32_t vertexBuffersBinding = 6;
        constexpr std::uint32_t indexBuffersBinding = 7;
        constexpr std::uint32_t instancesBinding = 8;
        constexpr std::uint32_t materialsBinding = 9;
        constexpr std::uint32_t diffuseOutputBinding = 10;
        constexpr std::uint32_t specularOutputBinding = 11;
        constexpr std::uint32_t viewZOutputBinding = 12;
        constexpr std::uint32_t denoiserNormalRoughnessOutputBinding = 13;
        constexpr std::uint32_t denoiserMotionOutputBinding = 14;
        constexpr std::uint32_t constantsBinding = 15;
        constexpr std::uint32_t sharcHashEntriesBinding = 16;
        constexpr std::uint32_t sharcAccumulationBinding = 17;
        constexpr std::uint32_t sharcResolvedBinding = 18;
        constexpr std::uint32_t sharcLockBinding = 19;
        constexpr std::uint32_t sharcStatisticsBinding = 20;
        constexpr std::uint32_t sharcConstantsBinding = 21;
        constexpr std::uint32_t baseColorTexturesBinding = 22;
        constexpr std::uint32_t normalRoughnessTexturesBinding = 23;
        constexpr std::uint32_t materialSamplerBinding = 24;
        constexpr std::uint32_t lightsBinding = 25;

        [[nodiscard]] bool complete(const SharcIndirectLightingFrameInputs& inputs) noexcept {
            return inputs.position && inputs.normalRoughness && inputs.albedoMetallic && inputs.motion &&
                   inputs.materialId;
        }

        [[nodiscard]] bool complete(const SharcIndirectLightingFrameResources& resources) noexcept {
            return resources.diffuseRadianceHitDistance && resources.specularRadianceHitDistance && resources.viewZ &&
                   resources.normalRoughness && resources.motion && resources.constants;
        }

        [[nodiscard]] bool complete(const SharcIndirectLightingSceneBindings& scene) noexcept {
            return scene.descriptors.rayTracingEnabled && scene.descriptors.tlas && scene.descriptors.instances &&
                   scene.descriptors.materials && scene.descriptors.lights && scene.descriptors.lightCount > 0 &&
                   !scene.geometry.empty() && !scene.baseColorTextures.empty() &&
                   scene.baseColorTextures.size() == scene.normalRoughnessTextures.size() && scene.materialSampler;
        }

        [[nodiscard]] nvrhi::BufferHandle createConstantBuffer(nvrhi::IDevice& device) {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(SharcIndirectLightingConstants);
            desc.debugName = "SHARC indirect lighting constants";
            desc.isConstantBuffer = true;
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            nvrhi::BufferHandle result = device.createBuffer(desc);
            if (!result) {
                throw std::runtime_error("Failed to create SHARC indirect lighting constants buffer.");
            }
            return result;
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer,
                            const SharcIndirectLightingConstants& constants) {
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map SHARC indirect lighting constants buffer.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

        [[nodiscard]] nvrhi::BindingSetDesc makeBindingSetDesc(const SharcIndirectLightingFrameInputs& inputs,
                                                               const SharcIndirectLightingFrameResources& resources,
                                                               const SharcIndirectLightingSceneBindings& scene,
                                                               std::uint32_t maxGeometryDescriptors,
                                                               std::uint32_t maxMaterialTextureDescriptors,
                                                               const SharcGraphRecord& sharc) {
            if (!complete(inputs) || !complete(resources) || !complete(scene) || !sharc.isValid() ||
                scene.geometry.size() > maxGeometryDescriptors ||
                scene.baseColorTextures.size() > maxMaterialTextureDescriptors) {
                throw std::invalid_argument("SHARC indirect lighting binding set resources are incomplete.");
            }

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(tlasBinding, scene.descriptors.tlas))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(positionBinding, inputs.position))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(normalRoughnessInputBinding, inputs.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(albedoMetallicBinding, inputs.albedoMetallic))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(motionInputBinding, inputs.motion))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(materialIdInputBinding, inputs.materialId))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(instancesBinding, scene.descriptors.instances))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(materialsBinding, scene.descriptors.materials))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(diffuseOutputBinding, resources.diffuseRadianceHitDistance))
                .addItem(
                    nvrhi::BindingSetItem::Texture_UAV(specularOutputBinding, resources.specularRadianceHitDistance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(viewZOutputBinding, resources.viewZ))
                .addItem(
                    nvrhi::BindingSetItem::Texture_UAV(denoiserNormalRoughnessOutputBinding, resources.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(denoiserMotionOutputBinding, resources.motion))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, resources.constants))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcHashEntriesBinding, sharc.native.hashEntries))
                .addItem(
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcAccumulationBinding, sharc.native.accumulation))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcResolvedBinding, sharc.native.resolved))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcLockBinding, sharc.native.lock))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcStatisticsBinding, sharc.native.statistics))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(sharcConstantsBinding, sharc.native.constants))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(lightsBinding, scene.descriptors.lights));
            for (std::uint32_t index = 0; index < maxGeometryDescriptors; ++index) {
                const gpu::GpuGeometryDescriptor& geometry = scene.geometry[index % scene.geometry.size()];
                if (!geometry.vertices || !geometry.indices) {
                    throw std::invalid_argument("SHARC indirect lighting geometry descriptor is incomplete.");
                }
                desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(vertexBuffersBinding, geometry.vertices)
                                 .setArrayElement(index));
                desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(indexBuffersBinding, geometry.indices)
                                 .setArrayElement(index));
            }
            for (std::uint32_t index = 0; index < maxMaterialTextureDescriptors; ++index) {
                const std::size_t textureIndex = index % scene.baseColorTextures.size();
                desc.addItem(
                    nvrhi::BindingSetItem::Texture_SRV(baseColorTexturesBinding, scene.baseColorTextures[textureIndex])
                        .setArrayElement(index));
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(normalRoughnessTexturesBinding,
                                                                scene.normalRoughnessTextures[textureIndex])
                                 .setArrayElement(index));
            }
            desc.addItem(nvrhi::BindingSetItem::Sampler(materialSamplerBinding, scene.materialSampler));
            return desc;
        }

    } // namespace

    namespace detail {

        nvrhi::TextureDesc makeSharcIndirectLightingTextureDesc(std::uint32_t width, std::uint32_t height,
                                                                nvrhi::Format format, const char* debugName) {
            if (width == 0 || height == 0 || format == nvrhi::Format::UNKNOWN || debugName == nullptr ||
                debugName[0] == '\0') {
                throw std::invalid_argument("SHARC indirect lighting texture requires dimensions and debug name.");
            }
            nvrhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.debugName = debugName;
            desc.isShaderResource = true;
            desc.isUAV = true;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            return desc;
        }

        nvrhi::BindingLayoutDesc
        makeSharcIndirectLightingBindingLayoutDesc(std::uint32_t maxGeometryDescriptors,
                                                   std::uint32_t maxMaterialTextureDescriptors) {
            if (maxGeometryDescriptors == 0 || maxGeometryDescriptors > std::numeric_limits<std::uint16_t>::max() ||
                maxMaterialTextureDescriptors == 0 ||
                maxMaterialTextureDescriptors > std::numeric_limits<std::uint16_t>::max()) {
                throw std::invalid_argument(
                    "SHARC indirect lighting descriptor capacities must fit NvRHI's uint16 array size.");
            }
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::AllRayTracing)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                       .setShaderResourceOffset(0)
                                       .setSamplerOffset(0)
                                       .setConstantBufferOffset(0)
                                       .setUnorderedAccessViewOffset(0))
                .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(tlasBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(positionBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(normalRoughnessInputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(albedoMetallicBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(motionInputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(materialIdInputBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(vertexBuffersBinding)
                             .setSize(maxGeometryDescriptors))
                .addItem(
                    nvrhi::BindingLayoutItem::StructuredBuffer_SRV(indexBuffersBinding).setSize(maxGeometryDescriptors))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(instancesBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(materialsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(diffuseOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(specularOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(viewZOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(denoiserNormalRoughnessOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(denoiserMotionOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcHashEntriesBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcAccumulationBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcResolvedBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcLockBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcStatisticsBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(sharcConstantsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(baseColorTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(normalRoughnessTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Sampler(materialSamplerBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(lightsBinding));
            return desc;
        }

    } // namespace detail

    struct SharcIndirectLightingPass::Impl {
        nvrhi::IDevice& device;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t maxGeometryDescriptors = 0;
        std::uint32_t maxMaterialTextureDescriptors = 0;
        std::vector<SharcIndirectLightingFrameInputs> inputs;
        std::vector<SharcIndirectLightingFrameResources> resources;
        std::vector<std::uint8_t> initialized;
        std::optional<std::uint32_t> pendingFrame;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::rt::PipelineHandle pipeline;
        nvrhi::rt::ShaderTableHandle shaderTable;

        explicit Impl(const SharcIndirectLightingCreateInfo& createInfo)
            : device(*createInfo.device), width(createInfo.width), height(createInfo.height),
              maxGeometryDescriptors(createInfo.maxGeometryDescriptors),
              maxMaterialTextureDescriptors(createInfo.maxMaterialTextureDescriptors),
              inputs(createInfo.frames.begin(), createInfo.frames.end()), initialized(createInfo.frames.size(), 0) {
            PipelineFactory pipelines(device);
            bindingLayout = device.createBindingLayout(detail::makeSharcIndirectLightingBindingLayoutDesc(
                maxGeometryDescriptors, maxMaterialTextureDescriptors));
            if (!bindingLayout) {
                throw std::runtime_error("Failed to create SHARC indirect lighting binding layout.");
            }
            const std::array shaderExports = {
                RayTracingPipelineShaderDesc{"RayGen",
                                             createInfo.shaders->load(ShaderId::SharcIndirectLightingRayGeneration)},
                RayTracingPipelineShaderDesc{"RadianceMiss",
                                             createInfo.shaders->load(ShaderId::SharcIndirectLightingRadianceMiss)},
                RayTracingPipelineShaderDesc{"ShadowMiss",
                                             createInfo.shaders->load(ShaderId::SharcIndirectLightingShadowMiss)},
            };
            const std::array hitGroups = {RayTracingHitGroupDesc{
                "TriangleHit", createInfo.shaders->load(ShaderId::SharcIndirectLightingClosestHit), nullptr, nullptr,
                false}};
            const std::array layouts = {bindingLayout, createInfo.atmosphereBindingLayout};
            pipeline = pipelines.createRayTracingPipeline(RayTracingPipelineDesc{.shaders = shaderExports,
                                                                                 .hitGroups = hitGroups,
                                                                                 .globalBindingLayouts = layouts,
                                                                                 .maxPayloadSize = 32,
                                                                                 .maxAttributeSize = sizeof(float) * 2,
                                                                                 .maxRecursionDepth = 2});
            const std::array missEntries = {RayTracingShaderTableEntryDesc{"RadianceMiss"},
                                            RayTracingShaderTableEntryDesc{"ShadowMiss"}};
            const std::array hitEntries = {RayTracingShaderTableEntryDesc{"TriangleHit"}};
            shaderTable = pipelines.createRayTracingShaderTable(
                pipeline, RayTracingShaderTableDesc{.rayGenerationExport = "RayGen",
                                                    .missShaders = missEntries,
                                                    .hitGroups = hitEntries,
                                                    .callableShaders = {},
                                                    .cached = true,
                                                    .maxEntries = 4,
                                                    .debugName = "SHARC indirect lighting shader table"});

            resources.reserve(inputs.size());
            const SharcIndirectLightingSignalFormats formats;
            for (std::size_t frameIndex = 0; frameIndex < inputs.size(); ++frameIndex) {
                const auto createTexture = [&](nvrhi::Format format, const char* debugName) {
                    nvrhi::TextureHandle texture = device.createTexture(
                        detail::makeSharcIndirectLightingTextureDesc(width, height, format, debugName));
                    if (!texture) {
                        throw std::runtime_error("Failed to create SHARC indirect lighting signal texture.");
                    }
                    return texture;
                };
                resources.push_back(SharcIndirectLightingFrameResources{
                    .diffuseRadianceHitDistance =
                        createTexture(formats.diffuseRadianceHitDistance, "SHARC indirect diffuse"),
                    .specularRadianceHitDistance =
                        createTexture(formats.specularRadianceHitDistance, "SHARC indirect specular"),
                    .viewZ = createTexture(formats.viewZ, "SHARC indirect view Z"),
                    .normalRoughness = createTexture(formats.normalRoughness, "SHARC indirect NRD normal roughness"),
                    .motion = createTexture(formats.motion, "SHARC indirect NRD motion"),
                    .constants = createConstantBuffer(device),
                });
            }
        }
    };

    SharcIndirectLightingPass::SharcIndirectLightingPass(const SharcIndirectLightingCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaders == nullptr || createInfo.width == 0 ||
            createInfo.height == 0 || createInfo.maxGeometryDescriptors == 0 ||
            createInfo.maxMaterialTextureDescriptors == 0 || !createInfo.atmosphereBindingLayout ||
            createInfo.frames.empty()) {
            throw std::invalid_argument(
                "SHARC indirect lighting requires device, shaders, extent, descriptor capacities, and frames.");
        }
        for (const SharcIndirectLightingFrameInputs& inputs : createInfo.frames) {
            if (!complete(inputs)) {
                throw std::invalid_argument("SHARC indirect lighting received incomplete primary surface inputs.");
            }
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    SharcIndirectLightingPass::~SharcIndirectLightingPass() = default;

    SharcIndirectLightingGraphOutput SharcIndirectLightingPass::record(
        FrameGraph& frameGraph, std::uint32_t frameIndex, bool frameSlotFenceWaited,
        const SharcIndirectLightingConstants& constants, const SharcIndirectLightingFrameGraphInputs& inputs,
        const SharcIndirectLightingSceneBindings& scene, const SharcIndirectLightingSceneGraphResources& sceneResources,
        const RayTracingEnvironmentBindings& environment,
        const RayTracingEnvironmentGraphResources& environmentResources, const SharcGraphRecord& sharc) {
        if (!frameSlotFenceWaited) {
            throw std::logic_error("SHARC indirect lighting may only update resources after the slot fence wait.");
        }
        if (impl_->pendingFrame) {
            throw std::logic_error("SHARC indirect lighting already has a pending frame.");
        }
        if (frameIndex >= impl_->resources.size()) {
            throw std::out_of_range("SHARC indirect lighting frame slot is out of range.");
        }
        if (!inputs.position.isValid() || !inputs.normalRoughness.isValid() || !inputs.albedoMetallic.isValid() ||
            !inputs.motion.isValid() || !inputs.materialId.isValid() || !sceneResources.tlas.isValid() ||
            !sceneResources.instances.isValid() || !sceneResources.materials.isValid() ||
            !sceneResources.lights.isValid() || !environment.isValid() || !environmentResources.isValid() ||
            !sharc.isValid() || sceneResources.vertices.size() != scene.geometry.size() ||
            sceneResources.indices.size() != scene.geometry.size() ||
            sceneResources.baseColorTextures.size() != scene.baseColorTextures.size() ||
            sceneResources.normalRoughnessTextures.size() != scene.normalRoughnessTextures.size()) {
            throw std::invalid_argument("SHARC indirect lighting requires matching native and graph resources.");
        }

        SharcIndirectLightingFrameResources& frameResources = impl_->resources[frameIndex];
        writeConstants(impl_->device, frameResources.constants, constants);
        const nvrhi::BindingSetHandle bindingSet = impl_->device.createBindingSet(
            makeBindingSetDesc(impl_->inputs[frameIndex], frameResources, scene, impl_->maxGeometryDescriptors,
                               impl_->maxMaterialTextureDescriptors, sharc),
            impl_->bindingLayout);
        if (!bindingSet) {
            throw std::runtime_error("Failed to create SHARC indirect lighting binding set.");
        }

        const auto importSignal = [&](const char* name, nvrhi::ITexture* texture) {
            return frameGraph.importTexture(
                std::string{name} + "-" + std::to_string(frameIndex),
                FrameGraphTextureDesc{.texture = texture,
                                      .initialState = impl_->initialized[frameIndex] != 0
                                                          ? nvrhi::ResourceStates::ShaderResource
                                                          : nvrhi::ResourceStates::Common,
                                      .finalState = nvrhi::ResourceStates::ShaderResource});
        };
        SharcIndirectLightingGraphOutput output{
            .diffuseRadianceHitDistance =
                importSignal("sharc-indirect-diffuse", frameResources.diffuseRadianceHitDistance),
            .specularRadianceHitDistance =
                importSignal("sharc-indirect-specular", frameResources.specularRadianceHitDistance),
            .viewZ = importSignal("sharc-indirect-view-z", frameResources.viewZ),
            .normalRoughness = importSignal("sharc-indirect-normal-roughness", frameResources.normalRoughness),
            .motion = importSignal("sharc-indirect-motion", frameResources.motion),
            .tracePass = {},
        };
        const FrameGraphResourceHandle constantsResource =
            frameGraph.importBuffer("sharc-indirect-lighting-constants-" + std::to_string(frameIndex),
                                    FrameGraphBufferDesc{.size = sizeof(SharcIndirectLightingConstants),
                                                         .buffer = frameResources.constants,
                                                         .initialState = impl_->initialized[frameIndex] != 0
                                                                             ? nvrhi::ResourceStates::ConstantBuffer
                                                                             : nvrhi::ResourceStates::Common,
                                                         .finalState = nvrhi::ResourceStates::ConstantBuffer});
        const std::vector<FrameGraphResourceHandle> vertices(sceneResources.vertices.begin(),
                                                             sceneResources.vertices.end());
        const std::vector<FrameGraphResourceHandle> indices(sceneResources.indices.begin(),
                                                            sceneResources.indices.end());
        const std::vector<FrameGraphResourceHandle> baseColors(sceneResources.baseColorTextures.begin(),
                                                               sceneResources.baseColorTextures.end());
        const std::vector<FrameGraphResourceHandle> normalRoughnessTextures(
            sceneResources.normalRoughnessTextures.begin(), sceneResources.normalRoughnessTextures.end());
        const nvrhi::rt::ShaderTableHandle shaderTable = impl_->shaderTable;
        const std::uint32_t width = impl_->width;
        const std::uint32_t height = impl_->height;
        output.tracePass = frameGraph.addPass(
            "sharc-indirect-lighting", FrameGraphPassType::RayTracing,
            [inputs, sceneResources, environmentResources, sharc, output, constantsResource, vertices, indices,
             baseColors, normalRoughnessTextures](FrameGraphBuilder& builder) {
                builder.dependsOn(sharc.resolvePass);
                if (sceneResources.readyPass.isValid()) {
                    builder.dependsOn(sceneResources.readyPass);
                }
                builder.readAccelerationStructure(sceneResources.tlas);
                builder.read(sceneResources.instances, nvrhi::ResourceStates::ShaderResource);
                builder.read(sceneResources.materials, nvrhi::ResourceStates::ShaderResource);
                builder.read(sceneResources.lights, nvrhi::ResourceStates::ShaderResource);
                for (const FrameGraphResourceHandle resource : vertices) {
                    builder.read(resource, nvrhi::ResourceStates::ShaderResource);
                }
                for (const FrameGraphResourceHandle resource : indices) {
                    builder.read(resource, nvrhi::ResourceStates::ShaderResource);
                }
                for (const FrameGraphResourceHandle resource : baseColors) {
                    builder.readTexture(resource, nvrhi::ResourceStates::ShaderResource);
                }
                for (const FrameGraphResourceHandle resource : normalRoughnessTextures) {
                    builder.readTexture(resource, nvrhi::ResourceStates::ShaderResource);
                }
                builder.readTexture(inputs.position);
                builder.readTexture(inputs.normalRoughness);
                builder.readTexture(inputs.albedoMetallic);
                builder.readTexture(inputs.motion);
                builder.readTexture(inputs.materialId);
                for (const FrameGraphResourceHandle lut : environmentResources.atmosphere.textures) {
                    builder.readTexture(lut);
                }
                builder.read(environmentResources.atmosphere.constants, nvrhi::ResourceStates::ConstantBuffer);
                builder.read(constantsResource, nvrhi::ResourceStates::ConstantBuffer);
                builder.read(sharc.resources.hashEntries, nvrhi::ResourceStates::UnorderedAccess);
                builder.read(sharc.resources.accumulation, nvrhi::ResourceStates::UnorderedAccess);
                builder.read(sharc.resources.resolved, nvrhi::ResourceStates::UnorderedAccess);
                builder.read(sharc.resources.lock, nvrhi::ResourceStates::UnorderedAccess);
                builder.read(sharc.resources.constants, nvrhi::ResourceStates::ConstantBuffer);
                builder.readWrite(sharc.resources.statistics, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.diffuseRadianceHitDistance, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.specularRadianceHitDistance, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.viewZ, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.normalRoughness, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.motion, nvrhi::ResourceStates::UnorderedAccess);
            },
            [bindingSet, shaderTable, atmosphere = environment.atmosphere, width,
             height](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("SHARC indirect lighting requires an NvRHI command list.");
                }
                nvrhi::rt::State state;
                state.setShaderTable(shaderTable).addBindingSet(bindingSet).addBindingSet(atmosphere);
                detail::recordSharcIndirectLightingDispatch(*context.commandList, state, width, height);
            });
        impl_->pendingFrame = frameIndex;
        return output;
    }

    const SharcIndirectLightingFrameResources& SharcIndirectLightingPass::resources(std::uint32_t frameIndex) const {
        if (frameIndex >= impl_->resources.size()) {
            throw std::out_of_range("SHARC indirect lighting frame slot is out of range.");
        }
        return impl_->resources[frameIndex];
    }

    SharcIndirectLightingSignalFormats SharcIndirectLightingPass::formats() const noexcept {
        return {};
    }

    SharcIndirectLightingGraphOutput SharcIndirectLightingPass::recordClear(FrameGraph& frameGraph,
                                                                            std::uint32_t frameIndex,
                                                                            bool frameSlotFenceWaited) {
        if (!frameSlotFenceWaited) {
            throw std::logic_error("SHARC indirect clear may only run after the slot fence wait.");
        }
        if (impl_->pendingFrame) {
            throw std::logic_error("SHARC indirect lighting already has a pending frame.");
        }
        if (frameIndex >= impl_->resources.size()) {
            throw std::out_of_range("SHARC indirect lighting frame slot is out of range.");
        }
        const SharcIndirectLightingFrameResources& resources = impl_->resources[frameIndex];
        const auto importSignal = [&](const char* name, nvrhi::ITexture* texture) {
            return frameGraph.importTexture(
                std::string{name} + "-" + std::to_string(frameIndex),
                FrameGraphTextureDesc{.texture = texture,
                                      .initialState = impl_->initialized[frameIndex] != 0
                                                          ? nvrhi::ResourceStates::ShaderResource
                                                          : nvrhi::ResourceStates::Common,
                                      .finalState = nvrhi::ResourceStates::ShaderResource});
        };
        SharcIndirectLightingGraphOutput output{
            .diffuseRadianceHitDistance = importSignal("sharc-indirect-diffuse", resources.diffuseRadianceHitDistance),
            .specularRadianceHitDistance =
                importSignal("sharc-indirect-specular", resources.specularRadianceHitDistance),
            .viewZ = importSignal("sharc-indirect-view-z", resources.viewZ),
            .normalRoughness = importSignal("sharc-indirect-normal-roughness", resources.normalRoughness),
            .motion = importSignal("sharc-indirect-motion", resources.motion),
            .tracePass = {},
        };
        const std::array textures = {resources.diffuseRadianceHitDistance, resources.specularRadianceHitDistance,
                                     resources.viewZ, resources.normalRoughness, resources.motion};
        const FrameGraphPassHandle clearPass = frameGraph.addPass(
            "sharc-indirect-lighting-clear", FrameGraphPassType::Transfer,
            [output](FrameGraphBuilder& builder) {
                builder.writeTexture(output.diffuseRadianceHitDistance, nvrhi::ResourceStates::CopyDest);
                builder.writeTexture(output.specularRadianceHitDistance, nvrhi::ResourceStates::CopyDest);
                builder.writeTexture(output.viewZ, nvrhi::ResourceStates::CopyDest);
                builder.writeTexture(output.normalRoughness, nvrhi::ResourceStates::CopyDest);
                builder.writeTexture(output.motion, nvrhi::ResourceStates::CopyDest);
            },
            [textures](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("SHARC indirect clear requires an NvRHI command list.");
                }
                for (const nvrhi::TextureHandle& texture : textures) {
                    context.commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color{0.0F});
                }
            });
        impl_->pendingFrame = frameIndex;
        output.tracePass = clearPass;
        return output;
    }

    void SharcIndirectLightingPass::commitSubmittedFrame() {
        if (impl_->pendingFrame) {
            impl_->initialized[*impl_->pendingFrame] = 1;
            impl_->pendingFrame.reset();
        }
    }

    void SharcIndirectLightingPass::discardPendingFrame() noexcept {
        impl_->pendingFrame.reset();
    }

    bool SharcIndirectLightingPass::hasPendingFrame() const noexcept {
        return impl_->pendingFrame.has_value();
    }

} // namespace lumin::render::gi

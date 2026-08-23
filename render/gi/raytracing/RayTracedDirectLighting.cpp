#include "render/gi/raytracing/RayTracedDirectLighting.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"
#include "scene/Environment.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lumin::render::gi {
    namespace {

        constexpr float rasterReferenceSunRadiance = 3.2F;
        constexpr float referenceSunIlluminanceLux = 110000.0F;
        constexpr float directLuxToRendererRadiance = rasterReferenceSunRadiance / referenceSunIlluminanceLux;

        constexpr std::uint32_t tlasBinding = 0;
        constexpr std::uint32_t vertexBuffersBinding = 1;
        constexpr std::uint32_t indexBuffersBinding = 2;
        constexpr std::uint32_t instancesBinding = 3;
        constexpr std::uint32_t materialsBinding = 4;
        constexpr std::uint32_t worldPositionOutputBinding = 5;
        constexpr std::uint32_t normalRoughnessOutputBinding = 6;
        constexpr std::uint32_t albedoMetallicOutputBinding = 7;
        constexpr std::uint32_t materialIdOutputBinding = 8;
        constexpr std::uint32_t viewZOutputBinding = 9;
        constexpr std::uint32_t motionOutputBinding = 10;
        constexpr std::uint32_t directRadianceOutputBinding = 11;
        constexpr std::uint32_t visibilityMaskOutputBinding = 12;
        constexpr std::uint32_t constantsBinding = 13;
        constexpr std::uint32_t baseColorTexturesBinding = 14;
        constexpr std::uint32_t normalRoughnessTexturesBinding = 15;
        constexpr std::uint32_t materialSamplerBinding = 16;

        [[nodiscard]] nvrhi::BufferHandle createConstantBuffer(nvrhi::IDevice& device) {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(RayTracedDiConstants);
            desc.debugName = "RT surface constants";
            desc.isConstantBuffer = true;
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            nvrhi::BufferHandle result = device.createBuffer(desc);
            if (!result) {
                throw std::runtime_error("Failed to create RT surface constants buffer.");
            }
            return result;
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer, const RayTracedDiConstants& constants) {
            if (buffer == nullptr) {
                throw std::invalid_argument("RT surface constants buffer is null.");
            }
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map RT surface constants buffer.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

        [[nodiscard]] nvrhi::TextureHandle createSignalTexture(nvrhi::IDevice& device, std::uint32_t width,
                                                               std::uint32_t height, nvrhi::Format format,
                                                               const char* debugName) {
            nvrhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.debugName = debugName;
            desc.isShaderResource = true;
            desc.isUAV = true;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            nvrhi::TextureHandle texture = device.createTexture(desc);
            if (!texture) {
                throw std::runtime_error(std::string{"Failed to create RT surface texture: "} + debugName);
            }
            return texture;
        }

        [[nodiscard]] bool complete(const RayTracedDiFrameResources& frame) noexcept {
            return frame.isValid();
        }

    } // namespace

    glm::vec4 makeRayTracingSunRadiance(const scene::DirectionalLight& sun, bool directLightingEnabled) noexcept {
        // 尚无物理相机曝光；RT 直射光必须沿用 Raster 的 HDR 标尺，避免切换拓扑时能量骤降。
        const float directScale = directLightingEnabled ? sun.illuminanceLux * directLuxToRendererRadiance : 0.0F;
        return glm::vec4{sun.color * directScale, 1.0F};
    }

    namespace detail {

        nvrhi::BindingLayoutDesc makeRayTracedDiBindingLayoutDesc(std::uint32_t maxGeometryDescriptors,
                                                                  std::uint32_t maxMaterialTextureDescriptors) {
            if (maxGeometryDescriptors == 0 || maxGeometryDescriptors > std::numeric_limits<std::uint16_t>::max() ||
                maxMaterialTextureDescriptors == 0 ||
                maxMaterialTextureDescriptors > std::numeric_limits<std::uint16_t>::max()) {
                throw std::invalid_argument("RT surface descriptor capacities must fit NvRHI's uint16 array size.");
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
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(vertexBuffersBinding)
                             .setSize(maxGeometryDescriptors))
                .addItem(
                    nvrhi::BindingLayoutItem::StructuredBuffer_SRV(indexBuffersBinding).setSize(maxGeometryDescriptors))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(instancesBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(materialsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(worldPositionOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(normalRoughnessOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(albedoMetallicOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(materialIdOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(viewZOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(motionOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(directRadianceOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(visibilityMaskOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(baseColorTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(normalRoughnessTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Sampler(materialSamplerBinding));
            return desc;
        }

        nvrhi::BindingSetDesc makeRayTracedDiBindingSetDesc(const RayTracedDiFrameResources& outputs,
                                                            const RayTracedGiSceneBindings& scene,
                                                            std::uint32_t maxGeometryDescriptors,
                                                            std::uint32_t maxMaterialTextureDescriptors,
                                                            nvrhi::BufferHandle constants) {
            if (!complete(outputs) || !scene.descriptors.rayTracingEnabled || !scene.descriptors.tlas ||
                !scene.descriptors.instances || !scene.descriptors.materials || scene.geometry.empty() ||
                scene.geometry.size() > maxGeometryDescriptors || scene.baseColorTextures.empty() ||
                scene.baseColorTextures.size() != scene.normalRoughnessTextures.size() ||
                scene.baseColorTextures.size() > maxMaterialTextureDescriptors || !scene.materialSampler ||
                !constants) {
                throw std::invalid_argument(
                    "RT surface binding set requires a complete traceable GPU scene and outputs.");
            }

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(tlasBinding, scene.descriptors.tlas))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(instancesBinding, scene.descriptors.instances))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(materialsBinding, scene.descriptors.materials))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(worldPositionOutputBinding, outputs.worldPositionHitT))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(normalRoughnessOutputBinding, outputs.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(albedoMetallicOutputBinding, outputs.albedoMetallic))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(materialIdOutputBinding, outputs.materialId))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(viewZOutputBinding, outputs.viewZ))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(motionOutputBinding, outputs.motion))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(directRadianceOutputBinding, outputs.directRadiance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(visibilityMaskOutputBinding, outputs.visibilityMask))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, constants));
            for (std::uint32_t index = 0; index < maxGeometryDescriptors; ++index) {
                const gpu::GpuGeometryDescriptor& geometry = scene.geometry[index < scene.geometry.size() ? index : 0];
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

    } // namespace detail

    struct RayTracedDirectLightingPass::Impl {
        nvrhi::IDevice& device;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t maxGeometryDescriptors = 0;
        std::uint32_t maxMaterialTextureDescriptors = 0;
        std::vector<RayTracedDiFrameResources> frames;
        std::vector<nvrhi::BufferHandle> constants;
        std::vector<std::uint8_t> initialized;
        std::optional<std::uint32_t> pendingFrame;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::rt::PipelineHandle pipeline;
        nvrhi::rt::ShaderTableHandle shaderTable;

        explicit Impl(const CreateInfo& createInfo)
            : device(*createInfo.device), width(createInfo.width), height(createInfo.height),
              maxGeometryDescriptors(createInfo.maxGeometryDescriptors),
              maxMaterialTextureDescriptors(createInfo.maxMaterialTextureDescriptors),
              frames(createInfo.frames.begin(), createInfo.frames.end()), constants(frames.size()),
              initialized(frames.size(), 0) {
            for (RayTracedDiFrameResources& frame : frames) {
                if (!frame.viewZ) {
                    frame.viewZ =
                        createSignalTexture(device, width, height, nvrhi::Format::R32_FLOAT, "RT surface viewZ");
                }
                if (!frame.visibilityMask) {
                    frame.visibilityMask = createSignalTexture(device, width, height, nvrhi::Format::R32_UINT,
                                                               "RT surface visibility mask");
                }
                if (!complete(frame)) {
                    throw std::invalid_argument("RT surface pass received incomplete output textures.");
                }
            }

            ShaderLibrary shaders(device, createInfo.shaderDirectory);
            PipelineFactory pipelines(device);
            bindingLayout = device.createBindingLayout(
                detail::makeRayTracedDiBindingLayoutDesc(maxGeometryDescriptors, maxMaterialTextureDescriptors));
            if (!bindingLayout) {
                throw std::runtime_error("Failed to create RT surface binding layout.");
            }

            const nvrhi::ShaderHandle rayGeneration =
                shaders.loadRayTracingModule("RtDi.rgen.spv", nvrhi::ShaderType::RayGeneration, "rayGenerationMain");
            const nvrhi::ShaderHandle radianceMiss =
                shaders.loadRayTracingModule("RtDi.radiance.rmiss.spv", nvrhi::ShaderType::Miss, "primaryMissMain");
            const nvrhi::ShaderHandle shadowMiss =
                shaders.loadRayTracingModule("RtDi.shadow.rmiss.spv", nvrhi::ShaderType::Miss, "shadowMissMain");
            const nvrhi::ShaderHandle closestHit =
                shaders.loadRayTracingModule("RtDi.rchit.spv", nvrhi::ShaderType::ClosestHit, "primaryClosestHitMain");
            const std::array shaderExports = {
                RayTracingPipelineShaderDesc{"RayGen", rayGeneration},
                RayTracingPipelineShaderDesc{"PrimaryMiss", radianceMiss},
                RayTracingPipelineShaderDesc{"ShadowMiss", shadowMiss},
            };
            const std::array hitGroups = {
                RayTracingHitGroupDesc{"PrimaryHit", closestHit, nullptr, nullptr, false},
            };
            const std::array layouts = {bindingLayout, createInfo.atmosphereBindingLayout};
            pipeline = pipelines.createRayTracingPipeline(RayTracingPipelineDesc{
                .shaders = shaderExports,
                .hitGroups = hitGroups,
                .globalBindingLayouts = layouts,
                .maxPayloadSize = 64,
                .maxAttributeSize = sizeof(float) * 2,
                .maxRecursionDepth = 2,
            });

            const std::array missEntries = {
                RayTracingShaderTableEntryDesc{"PrimaryMiss"},
                RayTracingShaderTableEntryDesc{"ShadowMiss"},
            };
            const std::array hitEntries = {RayTracingShaderTableEntryDesc{"PrimaryHit"}};
            shaderTable = pipelines.createRayTracingShaderTable(
                pipeline, RayTracingShaderTableDesc{.rayGenerationExport = "RayGen",
                                                    .missShaders = missEntries,
                                                    .hitGroups = hitEntries,
                                                    .callableShaders = {},
                                                    .cached = true,
                                                    .maxEntries = 4,
                                                    .debugName = "RT primary surface shader table"});
            for (std::size_t index = 0; index < constants.size(); ++index) {
                constants[index] = createConstantBuffer(device);
            }
        }
    };

    RayTracedDirectLightingPass::RayTracedDirectLightingPass(const CreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaderDirectory.empty() || createInfo.width == 0 ||
            createInfo.height == 0 || createInfo.maxGeometryDescriptors == 0 ||
            createInfo.maxMaterialTextureDescriptors == 0 || !createInfo.atmosphereBindingLayout ||
            createInfo.frames.empty()) {
            throw std::invalid_argument("RT surface pass requires device, shader directory, extent, geometry capacity, "
                                        "atmosphere, and frames.");
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    RayTracedDirectLightingPass::~RayTracedDirectLightingPass() = default;

    FrameGraphPassHandle
    RayTracedDirectLightingPass::record(FrameGraph& frameGraph, std::uint32_t frameIndex, bool frameSlotFenceWaited,
                                        const RayTracedDiConstants& constants, const RayTracedDiGraphResources& outputs,
                                        const RayTracedGiSceneBindings& scene,
                                        const RayTracedGiSceneGraphResources& sceneResources,
                                        const RayTracingEnvironmentBindings& environment,
                                        const RayTracingEnvironmentGraphResources& environmentResources) {
        if (!frameSlotFenceWaited) {
            throw std::logic_error("RT surface bindings may only change after waiting for the frame-slot fence.");
        }
        if (impl_->pendingFrame) {
            throw std::logic_error("RT surface pass already has a pending frame.");
        }
        if (frameIndex >= impl_->frames.size() || !outputs.isValid() || !sceneResources.tlas.isValid() ||
            !sceneResources.instances.isValid() || !sceneResources.materials.isValid() ||
            sceneResources.vertices.size() != scene.geometry.size() ||
            sceneResources.indices.size() != scene.geometry.size() ||
            sceneResources.baseColorTextures.size() != scene.baseColorTextures.size() ||
            sceneResources.normalRoughnessTextures.size() != scene.normalRoughnessTextures.size() ||
            !environment.isValid() || !environmentResources.isValid()) {
            throw std::invalid_argument("RT surface record requires matching scene, output, and atmosphere resources.");
        }

        nvrhi::BufferHandle& frameConstants = impl_->constants[frameIndex];
        writeConstants(impl_->device, frameConstants, constants);
        const nvrhi::BindingSetHandle bindingSet = impl_->device.createBindingSet(
            detail::makeRayTracedDiBindingSetDesc(impl_->frames[frameIndex], scene, impl_->maxGeometryDescriptors,
                                                  impl_->maxMaterialTextureDescriptors, frameConstants),
            impl_->bindingLayout);
        if (!bindingSet) {
            throw std::runtime_error("Failed to create RT surface binding set for the current GPU Scene version.");
        }

        const std::vector<FrameGraphResourceHandle> vertexResources(sceneResources.vertices.begin(),
                                                                    sceneResources.vertices.end());
        const std::vector<FrameGraphResourceHandle> indexResources(sceneResources.indices.begin(),
                                                                   sceneResources.indices.end());
        const std::vector<FrameGraphResourceHandle> baseColorResources(sceneResources.baseColorTextures.begin(),
                                                                       sceneResources.baseColorTextures.end());
        const std::vector<FrameGraphResourceHandle> normalRoughnessResources(
            sceneResources.normalRoughnessTextures.begin(), sceneResources.normalRoughnessTextures.end());
        const nvrhi::rt::ShaderTableHandle shaderTable = impl_->shaderTable;
        const std::uint32_t width = impl_->width;
        const std::uint32_t height = impl_->height;
        const FrameGraphPassHandle pass = frameGraph.addPass(
            "rt-primary-surface", FrameGraphPassType::RayTracing,
            [outputs, sceneResources, environmentResources, vertexResources, indexResources, baseColorResources,
             normalRoughnessResources](FrameGraphBuilder& builder) {
                if (sceneResources.readyPass.isValid()) {
                    builder.dependsOn(sceneResources.readyPass);
                }
                builder.readAccelerationStructure(sceneResources.tlas);
                builder.read(sceneResources.instances, nvrhi::ResourceStates::ShaderResource);
                builder.read(sceneResources.materials, nvrhi::ResourceStates::ShaderResource);
                for (const FrameGraphResourceHandle resource : vertexResources) {
                    builder.read(resource, nvrhi::ResourceStates::ShaderResource);
                }
                for (const FrameGraphResourceHandle resource : indexResources) {
                    builder.read(resource, nvrhi::ResourceStates::ShaderResource);
                }
                for (const FrameGraphResourceHandle resource : baseColorResources) {
                    builder.readTexture(resource, nvrhi::ResourceStates::ShaderResource);
                }
                for (const FrameGraphResourceHandle resource : normalRoughnessResources) {
                    builder.readTexture(resource, nvrhi::ResourceStates::ShaderResource);
                }
                for (const FrameGraphResourceHandle lut : environmentResources.atmosphere.textures) {
                    builder.readTexture(lut, nvrhi::ResourceStates::ShaderResource);
                }
                builder.read(environmentResources.atmosphere.constants, nvrhi::ResourceStates::ConstantBuffer);
                for (const FrameGraphResourceHandle output :
                     {outputs.worldPositionHitT, outputs.normalRoughness, outputs.albedoMetallic, outputs.materialId,
                      outputs.viewZ, outputs.motion, outputs.directRadiance, outputs.visibilityMask}) {
                    builder.writeTexture(output, nvrhi::ResourceStates::UnorderedAccess);
                }
            },
            [atmosphereBindingSet = environment.atmosphere, bindingSet, shaderTable, width,
             height](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("RT surface dispatch requires an NvRHI command list.");
                }
                nvrhi::rt::State state;
                state.setShaderTable(shaderTable).addBindingSet(bindingSet).addBindingSet(atmosphereBindingSet);
                detail::recordRayTracedDiDispatch(*context.commandList, state, width, height);
            });
        impl_->pendingFrame = frameIndex;
        return pass;
    }

    void RayTracedDirectLightingPass::commitSubmittedFrame() {
        if (!impl_->pendingFrame) {
            return;
        }
        impl_->initialized[*impl_->pendingFrame] = 1;
        impl_->pendingFrame.reset();
    }

    void RayTracedDirectLightingPass::discardPendingFrame() noexcept {
        impl_->pendingFrame.reset();
    }

    bool RayTracedDirectLightingPass::hasPendingFrame() const noexcept {
        return impl_->pendingFrame.has_value();
    }

    const RayTracedDiFrameResources& RayTracedDirectLightingPass::signals(std::uint32_t frameIndex) const {
        if (frameIndex >= impl_->frames.size()) {
            throw std::out_of_range("RT surface frame index is outside the configured range.");
        }
        return impl_->frames[frameIndex];
    }

} // namespace lumin::render::gi

#include "render/gi/raytracing/RayTracedGi.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

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

        constexpr std::uint32_t tlasBinding = 0;
        constexpr std::uint32_t positionBinding = 1;
        constexpr std::uint32_t normalRoughnessInputBinding = 2;
        constexpr std::uint32_t albedoMetallicBinding = 3;
        constexpr std::uint32_t motionInputBinding = 4;
        constexpr std::uint32_t vertexBuffersBinding = 5;
        constexpr std::uint32_t indexBuffersBinding = 6;
        constexpr std::uint32_t instancesBinding = 7;
        constexpr std::uint32_t materialsBinding = 8;
        constexpr std::uint32_t diffuseOutputBinding = 9;
        constexpr std::uint32_t specularOutputBinding = 10;
        constexpr std::uint32_t viewZOutputBinding = 11;
        constexpr std::uint32_t normalRoughnessOutputBinding = 12;
        constexpr std::uint32_t motionOutputBinding = 13;
        constexpr std::uint32_t constantsBinding = 14;
        constexpr std::uint32_t sharcHashEntriesBinding = 15;
        constexpr std::uint32_t sharcAccumulationBinding = 16;
        constexpr std::uint32_t sharcResolvedBinding = 17;
        constexpr std::uint32_t sharcLockBinding = 18;
        constexpr std::uint32_t sharcStatisticsBinding = 19;
        constexpr std::uint32_t sharcConstantsBinding = 20;
        constexpr std::uint32_t baseColorTexturesBinding = 21;
        constexpr std::uint32_t normalRoughnessTexturesBinding = 22;
        constexpr std::uint32_t materialSamplerBinding = 23;

        [[nodiscard]] bool complete(const RayTracedGiFrameInputs& inputs) noexcept {
            return inputs.position && inputs.normalRoughness && inputs.albedoMetallic && inputs.motion;
        }

        [[nodiscard]] bool complete(const RayTracedGiSignalResources& signals) noexcept {
            return signals.diffuseRadianceHitDistance && signals.specularRadianceHitDistance && signals.viewZ &&
                   signals.normalRoughness && signals.motion && signals.constants;
        }

        [[nodiscard]] nvrhi::BufferHandle createConstantBuffer(nvrhi::IDevice& device) {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(RayTracedGiConstants);
            desc.debugName = "RT GI constants";
            desc.isConstantBuffer = true;
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            nvrhi::BufferHandle result = device.createBuffer(desc);
            if (!result) {
                throw std::runtime_error("Failed to create RT GI constants buffer.");
            }
            return result;
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer, const RayTracedGiConstants& constants) {
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map RT GI constants buffer.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

    } // namespace

    namespace detail {

        nvrhi::TextureDesc makeRayTracedGiSignalTextureDesc(std::uint32_t width, std::uint32_t height,
                                                            nvrhi::Format format, const char* debugName) {
            if (width == 0 || height == 0 || format == nvrhi::Format::UNKNOWN || debugName == nullptr ||
                debugName[0] == '\0') {
                throw std::invalid_argument("RT GI signals require dimensions, format, and debug name.");
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

        nvrhi::BindingLayoutDesc makeRayTracedGiBindingLayoutDesc(std::uint32_t maxGeometryDescriptors,
                                                                  bool enableSharc,
                                                                  std::uint32_t maxMaterialTextureDescriptors) {
            if (maxGeometryDescriptors == 0 || maxGeometryDescriptors > std::numeric_limits<std::uint16_t>::max() ||
                maxMaterialTextureDescriptors == 0 ||
                maxMaterialTextureDescriptors > std::numeric_limits<std::uint16_t>::max()) {
                throw std::invalid_argument("RT GI descriptor capacities must fit NvRHI's uint16 array size.");
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
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(vertexBuffersBinding)
                             .setSize(maxGeometryDescriptors))
                .addItem(
                    nvrhi::BindingLayoutItem::StructuredBuffer_SRV(indexBuffersBinding).setSize(maxGeometryDescriptors))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(instancesBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(materialsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(diffuseOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(specularOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(viewZOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(normalRoughnessOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(motionOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding));
            if (enableSharc) {
                desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcHashEntriesBinding))
                    .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcAccumulationBinding))
                    .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcResolvedBinding))
                    .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcLockBinding))
                    .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(sharcStatisticsBinding))
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(sharcConstantsBinding));
            }
            desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(baseColorTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(normalRoughnessTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Sampler(materialSamplerBinding));
            return desc;
        }

        nvrhi::BindingSetDesc
        makeRayTracedGiBindingSetDesc(const RayTracedGiFrameInputs& inputs, const RayTracedGiSignalResources& signals,
                                      const RayTracedGiSceneBindings& scene, std::uint32_t maxGeometryDescriptors,
                                      std::uint32_t maxMaterialTextureDescriptors, const SharcGraphRecord* sharc) {
            if (!complete(inputs) || !complete(signals) || !scene.descriptors.rayTracingEnabled ||
                !scene.descriptors.tlas || !scene.descriptors.instances || !scene.descriptors.materials ||
                scene.geometry.empty() || scene.geometry.size() > maxGeometryDescriptors ||
                scene.baseColorTextures.empty() ||
                scene.baseColorTextures.size() != scene.normalRoughnessTextures.size() ||
                scene.baseColorTextures.size() > maxMaterialTextureDescriptors || !scene.materialSampler ||
                (sharc != nullptr && !sharc->isValid())) {
                throw std::invalid_argument("RT GI binding set requires a complete traceable GPU scene and signals.");
            }

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(tlasBinding, scene.descriptors.tlas))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(positionBinding, inputs.position))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(normalRoughnessInputBinding, inputs.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(albedoMetallicBinding, inputs.albedoMetallic))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(motionInputBinding, inputs.motion))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(instancesBinding, scene.descriptors.instances))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(materialsBinding, scene.descriptors.materials))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(diffuseOutputBinding, signals.diffuseRadianceHitDistance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(specularOutputBinding, signals.specularRadianceHitDistance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(viewZOutputBinding, signals.viewZ))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(normalRoughnessOutputBinding, signals.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(motionOutputBinding, signals.motion))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, signals.constants));
            if (sharc != nullptr) {
                desc.addItem(
                        nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcHashEntriesBinding, sharc->native.hashEntries))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcAccumulationBinding,
                                                                         sharc->native.accumulation))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcResolvedBinding, sharc->native.resolved))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcLockBinding, sharc->native.lock))
                    .addItem(
                        nvrhi::BindingSetItem::StructuredBuffer_UAV(sharcStatisticsBinding, sharc->native.statistics))
                    .addItem(nvrhi::BindingSetItem::ConstantBuffer(sharcConstantsBinding, sharc->native.constants));
            }
            for (std::uint32_t index = 0; index < maxGeometryDescriptors; ++index) {
                // NvRHI 的普通 BindingLayout 数组不是 bindless layout；所有静态数组元素都必须写入。
                const gpu::GpuGeometryDescriptor& geometry = scene.geometry[index % scene.geometry.size()];
                if (!geometry.vertices || !geometry.indices) {
                    throw std::invalid_argument("RT GI bindless geometry contains an incomplete descriptor.");
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

    } // namespace detail

    struct RayTracedGiPass::Impl {
        nvrhi::IDevice& device;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t maxGeometryDescriptors = 0;
        std::uint32_t maxMaterialTextureDescriptors = 0;
        bool sharcEnabled = false;
        std::vector<RayTracedGiFrameInputs> inputs;
        std::vector<RayTracedGiSignalResources> signals;
        std::vector<std::uint8_t> initialized;
        std::optional<std::uint32_t> pendingFrame;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::rt::PipelineHandle pipeline;
        nvrhi::rt::ShaderTableHandle shaderTable;

        explicit Impl(const RayTracedGiCreateInfo& createInfo)
            : device(*createInfo.device), width(createInfo.width), height(createInfo.height),
              maxGeometryDescriptors(createInfo.maxGeometryDescriptors),
              maxMaterialTextureDescriptors(createInfo.maxMaterialTextureDescriptors),
              sharcEnabled(createInfo.enableSharc), inputs(createInfo.frames.begin(), createInfo.frames.end()),
              initialized(createInfo.frames.size(), 0) {
            PipelineFactory pipelines(device);
            bindingLayout = device.createBindingLayout(detail::makeRayTracedGiBindingLayoutDesc(
                maxGeometryDescriptors, sharcEnabled, maxMaterialTextureDescriptors));
            if (!bindingLayout) {
                throw std::runtime_error("Failed to create RT GI binding layout.");
            }

            const nvrhi::ShaderHandle rayGeneration = createInfo.shaders->load(ShaderId::RtGiRayGeneration);
            const nvrhi::ShaderHandle radianceMiss = createInfo.shaders->load(ShaderId::RtGiRadianceMiss);
            const nvrhi::ShaderHandle shadowMiss = createInfo.shaders->load(ShaderId::RtGiShadowMiss);
            const nvrhi::ShaderHandle closestHit =
                createInfo.shaders->load(sharcEnabled ? ShaderId::RtGiSharcClosestHit : ShaderId::RtGiClosestHit);
            const std::array shaderExports = {
                RayTracingPipelineShaderDesc{"RayGen", rayGeneration},
                RayTracingPipelineShaderDesc{"RadianceMiss", radianceMiss},
                RayTracingPipelineShaderDesc{"ShadowMiss", shadowMiss},
            };
            const std::array hitGroups = {
                RayTracingHitGroupDesc{"TriangleHit", closestHit, nullptr, nullptr, false},
            };
            const std::array layouts = {bindingLayout, createInfo.atmosphereBindingLayout};
            RayTracingPipelineDesc pipelineDesc;
            pipelineDesc.shaders = shaderExports;
            pipelineDesc.hitGroups = hitGroups;
            pipelineDesc.globalBindingLayouts = layouts;
            pipelineDesc.maxPayloadSize = 32;
            pipelineDesc.maxAttributeSize = sizeof(float) * 2;
            pipelineDesc.maxRecursionDepth = 2;
            pipeline = pipelines.createRayTracingPipeline(pipelineDesc);

            const std::array missEntries = {
                RayTracingShaderTableEntryDesc{"RadianceMiss"},
                RayTracingShaderTableEntryDesc{"ShadowMiss"},
            };
            const std::array hitEntries = {RayTracingShaderTableEntryDesc{"TriangleHit"}};
            RayTracingShaderTableDesc tableDesc;
            tableDesc.rayGenerationExport = "RayGen";
            tableDesc.missShaders = missEntries;
            tableDesc.hitGroups = hitEntries;
            tableDesc.cached = true;
            tableDesc.maxEntries = 4;
            tableDesc.debugName = "RT GI shader table";
            shaderTable = pipelines.createRayTracingShaderTable(pipeline, tableDesc);

            signals.reserve(inputs.size());
            const RayTracedGiSignalFormats formats;
            for (std::size_t frameIndex = 0; frameIndex < inputs.size(); ++frameIndex) {
                RayTracedGiSignalResources frameSignals;
                const auto createTexture = [&](nvrhi::Format format, const char* name) {
                    nvrhi::TextureHandle texture =
                        device.createTexture(detail::makeRayTracedGiSignalTextureDesc(width, height, format, name));
                    if (!texture) {
                        throw std::runtime_error("Failed to create RT GI signal texture.");
                    }
                    return texture;
                };
                frameSignals.diffuseRadianceHitDistance =
                    createTexture(formats.diffuseRadianceHitDistance, "RT GI diffuse radiance hit distance");
                frameSignals.specularRadianceHitDistance =
                    createTexture(formats.specularRadianceHitDistance, "RT GI specular radiance hit distance");
                frameSignals.viewZ = createTexture(formats.viewZ, "RT GI viewZ");
                frameSignals.normalRoughness = createTexture(formats.normalRoughness, "RT GI normal roughness");
                frameSignals.motion = createTexture(formats.motion, "RT GI motion");
                frameSignals.constants = createConstantBuffer(device);
                signals.push_back(std::move(frameSignals));
            }
        }
    };

    RayTracedGiPass::RayTracedGiPass(const RayTracedGiCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaders == nullptr || createInfo.width == 0 ||
            createInfo.height == 0 || createInfo.maxGeometryDescriptors == 0 ||
            createInfo.maxMaterialTextureDescriptors == 0 || !createInfo.atmosphereBindingLayout ||
            createInfo.frames.empty()) {
            throw std::invalid_argument(
                "RT GI pass requires device, shader library, extent, geometry capacity, and frame inputs.");
        }
        for (const RayTracedGiFrameInputs& inputs : createInfo.frames) {
            if (!complete(inputs)) {
                throw std::invalid_argument("RT GI pass received an incomplete G-buffer frame.");
            }
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    RayTracedGiPass::~RayTracedGiPass() = default;

    RayTracedGiGraphSignals RayTracedGiPass::record(FrameGraph& frameGraph, std::uint32_t frameIndex,
                                                    bool frameSlotFenceWaited, const RayTracedGiConstants& constants,
                                                    const RayTracedGiFrameGraphInputs& inputs,
                                                    const RayTracedGiSceneBindings& scene,
                                                    const RayTracedGiSceneGraphResources& sceneResources,
                                                    const RayTracingEnvironmentBindings& environment,
                                                    const RayTracingEnvironmentGraphResources& environmentResources,
                                                    const SharcGraphRecord* sharc) {
        if (!frameSlotFenceWaited) {
            throw std::logic_error("RT GI constants and bindings may only change after waiting for the slot fence.");
        }
        if (impl_->pendingFrame) {
            throw std::logic_error("RT GI pass already has a pending frame.");
        }
        if (frameIndex >= impl_->signals.size() || frameIndex >= impl_->inputs.size()) {
            throw std::out_of_range("RT GI frame slot is outside the configured range.");
        }
        if (!inputs.position.isValid() || !inputs.normalRoughness.isValid() || !inputs.albedoMetallic.isValid() ||
            !inputs.motion.isValid() || !sceneResources.tlas.isValid() || !sceneResources.instances.isValid() ||
            !sceneResources.materials.isValid() || sceneResources.vertices.size() != scene.geometry.size() ||
            sceneResources.indices.size() != scene.geometry.size() ||
            sceneResources.baseColorTextures.size() != scene.baseColorTextures.size() ||
            sceneResources.normalRoughnessTextures.size() != scene.normalRoughnessTextures.size() ||
            !environment.isValid() || !environmentResources.isValid() || impl_->sharcEnabled != (sharc != nullptr)) {
            throw std::invalid_argument("RT GI record requires matching native and FrameGraph scene resources.");
        }

        RayTracedGiSignalResources& frameSignals = impl_->signals[frameIndex];
        writeConstants(impl_->device, frameSignals.constants, constants);
        nvrhi::BindingSetHandle bindingSet = impl_->device.createBindingSet(
            detail::makeRayTracedGiBindingSetDesc(impl_->inputs[frameIndex], frameSignals, scene,
                                                  impl_->maxGeometryDescriptors, impl_->maxMaterialTextureDescriptors,
                                                  sharc),
            impl_->bindingLayout);
        if (!bindingSet) {
            throw std::runtime_error("Failed to create RT GI binding set for the current GPU scene version.");
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
        RayTracedGiGraphSignals graphSignals{
            .diffuseRadianceHitDistance = importSignal("rt-gi-diffuse", frameSignals.diffuseRadianceHitDistance),
            .specularRadianceHitDistance = importSignal("rt-gi-specular", frameSignals.specularRadianceHitDistance),
            .viewZ = importSignal("rt-gi-view-z", frameSignals.viewZ),
            .normalRoughness = importSignal("rt-gi-normal-roughness", frameSignals.normalRoughness),
            .motion = importSignal("rt-gi-motion", frameSignals.motion),
            .tracePass = {},
        };
        const FrameGraphResourceHandle constantsResource =
            frameGraph.importBuffer("rt-gi-constants-" + std::to_string(frameIndex),
                                    FrameGraphBufferDesc{.size = sizeof(RayTracedGiConstants),
                                                         .buffer = frameSignals.constants,
                                                         .initialState = impl_->initialized[frameIndex] != 0
                                                                             ? nvrhi::ResourceStates::ConstantBuffer
                                                                             : nvrhi::ResourceStates::Common,
                                                         .finalState = nvrhi::ResourceStates::ConstantBuffer});

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
        graphSignals.tracePass = frameGraph.addPass(
            "gi-ray-trace", FrameGraphPassType::RayTracing,
            [inputs, sceneResources, environmentResources, graphSignals, constantsResource, vertexResources, sharc,
             indexResources, baseColorResources, normalRoughnessResources](FrameGraphBuilder& builder) {
                if (sceneResources.readyPass.isValid()) {
                    builder.dependsOn(sceneResources.readyPass);
                }
                if (sharc != nullptr) {
                    builder.dependsOn(sharc->resolvePass);
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
                builder.readTexture(inputs.position);
                builder.readTexture(inputs.normalRoughness);
                builder.readTexture(inputs.albedoMetallic);
                builder.readTexture(inputs.motion);
                for (const FrameGraphResourceHandle lut : environmentResources.atmosphere.textures) {
                    builder.readTexture(lut);
                }
                builder.read(environmentResources.atmosphere.constants, nvrhi::ResourceStates::ConstantBuffer);
                builder.read(constantsResource, nvrhi::ResourceStates::ConstantBuffer);
                if (sharc != nullptr) {
                    builder.read(sharc->resources.hashEntries, nvrhi::ResourceStates::UnorderedAccess);
                    builder.read(sharc->resources.accumulation, nvrhi::ResourceStates::UnorderedAccess);
                    builder.read(sharc->resources.resolved, nvrhi::ResourceStates::UnorderedAccess);
                    builder.read(sharc->resources.lock, nvrhi::ResourceStates::UnorderedAccess);
                    builder.read(sharc->resources.constants, nvrhi::ResourceStates::ConstantBuffer);
                    builder.readWrite(sharc->resources.statistics, nvrhi::ResourceStates::UnorderedAccess);
                }
                builder.writeTexture(graphSignals.diffuseRadianceHitDistance, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(graphSignals.specularRadianceHitDistance, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(graphSignals.viewZ, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(graphSignals.normalRoughness, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(graphSignals.motion, nvrhi::ResourceStates::UnorderedAccess);
            },
            [atmosphereBindingSet = environment.atmosphere, bindingSet, shaderTable, width,
             height](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("RT GI dispatch requires an NvRHI command list.");
                }
                nvrhi::rt::State state;
                state.setShaderTable(shaderTable).addBindingSet(bindingSet).addBindingSet(atmosphereBindingSet);
                detail::recordRayTracedGiDispatch(*context.commandList, state, width, height);
            });
        impl_->pendingFrame = frameIndex;
        return graphSignals;
    }

    const RayTracedGiSignalResources& RayTracedGiPass::signals(std::uint32_t frameIndex) const {
        if (frameIndex >= impl_->signals.size()) {
            throw std::out_of_range("RT GI frame slot is outside the configured range.");
        }
        return impl_->signals[frameIndex];
    }

    RayTracedGiSignalFormats RayTracedGiPass::formats() const noexcept {
        return {};
    }

    void RayTracedGiPass::commitSubmittedFrame() {
        if (!impl_->pendingFrame) {
            return;
        }
        impl_->initialized[*impl_->pendingFrame] = 1;
        impl_->pendingFrame.reset();
    }

    void RayTracedGiPass::discardPendingFrame() noexcept {
        impl_->pendingFrame.reset();
    }

    bool RayTracedGiPass::hasPendingFrame() const noexcept {
        return impl_->pendingFrame.has_value();
    }

} // namespace lumin::render::gi

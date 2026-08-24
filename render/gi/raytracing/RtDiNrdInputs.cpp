#include "render/gi/raytracing/RtDiNrdInputs.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lumin::render::gi {
    namespace {

        constexpr std::uint32_t diffuseInputBinding = 0;
        constexpr std::uint32_t specularInputBinding = 1;
        constexpr std::uint32_t positionBinding = 2;
        constexpr std::uint32_t normalRoughnessBinding = 3;
        constexpr std::uint32_t albedoMetallicBinding = 4;
        constexpr std::uint32_t materialIdBinding = 5;
        constexpr std::uint32_t viewZInputBinding = 6;
        constexpr std::uint32_t motionInputBinding = 7;
        constexpr std::uint32_t materialsBinding = 8;
        constexpr std::uint32_t constantsBinding = 9;
        constexpr std::uint32_t diffuseOutputBinding = 10;
        constexpr std::uint32_t specularOutputBinding = 11;
        constexpr std::uint32_t viewZOutputBinding = 12;
        constexpr std::uint32_t normalRoughnessOutputBinding = 13;
        constexpr std::uint32_t motionOutputBinding = 14;
        constexpr std::uint32_t threadGroupWidth = 8;
        constexpr std::uint32_t threadGroupHeight = 8;

        [[nodiscard]] bool floatFourChannel(nvrhi::Format format) noexcept {
            return format == nvrhi::Format::RGBA16_FLOAT || format == nvrhi::Format::RGBA32_FLOAT;
        }

        [[nodiscard]] bool albedoFormat(nvrhi::Format format) noexcept {
            return format == nvrhi::Format::RGBA8_UNORM || format == nvrhi::Format::BGRA8_UNORM ||
                   floatFourChannel(format);
        }

        template <typename Predicate>
        void validateTexture(nvrhi::ITexture* texture, core::RenderExtent extent, Predicate&& accepts,
                             const char* label) {
            if (texture == nullptr) {
                throw std::invalid_argument(std::string{"RTDI NRD preparation is missing "} + label + ".");
            }
            const nvrhi::TextureDesc& desc = texture->getDesc();
            if (desc.width != extent.width || desc.height != extent.height ||
                desc.dimension != nvrhi::TextureDimension::Texture2D || !desc.isShaderResource ||
                !accepts(desc.format)) {
                throw std::invalid_argument(std::string{"RTDI NRD preparation texture contract mismatch: "} + label +
                                            ".");
            }
        }

        [[nodiscard]] bool complete(const RtDiNrdInputGraphResources& resources) noexcept {
            return resources.diffuseRadianceHitT.isValid() && resources.specularRadianceHitT.isValid() &&
                   resources.position.isValid() && resources.normalRoughness.isValid() &&
                   resources.albedoMetallic.isValid() && resources.materialId.isValid() &&
                   resources.viewZ.isValid() && resources.motion.isValid() && resources.materials.isValid();
        }

        [[nodiscard]] nvrhi::BufferDesc makeConstantBufferDesc() {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(RtDiNrdInputsConstants);
            desc.debugName = "RTDI NRD preparation constants";
            desc.isConstantBuffer = true;
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
            desc.keepInitialState = false;
            return desc;
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer,
                            const RtDiNrdInputsConstants& constants) {
            if (buffer == nullptr) {
                throw std::invalid_argument("RTDI NRD preparation constants buffer is null.");
            }
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map RTDI NRD preparation constants.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

    } // namespace

    bool RtDiNrdGraphOutput::isValid() const noexcept {
        return diffuseRadianceHitDistance.isValid() && specularRadianceHitDistance.isValid() && viewZ.isValid() &&
               normalRoughness.isValid() && motion.isValid() && readyPass.isValid();
    }

    namespace detail {

        nvrhi::TextureDesc makeRtDiNrdSignalTextureDesc(std::uint32_t width, std::uint32_t height,
                                                        nvrhi::Format format, const char* debugName) {
            if (width == 0 || height == 0 || format == nvrhi::Format::UNKNOWN || debugName == nullptr) {
                throw std::invalid_argument("RTDI NRD signal textures require extent, format, and debug name.");
            }
            nvrhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.dimension = nvrhi::TextureDimension::Texture2D;
            desc.format = format;
            desc.debugName = debugName;
            desc.isShaderResource = true;
            desc.isUAV = true;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            return desc;
        }

        nvrhi::BindingLayoutDesc makeRtDiNrdInputsBindingLayoutDesc() {
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                       .setShaderResourceOffset(0)
                                       .setSamplerOffset(0)
                                       .setConstantBufferOffset(0)
                                       .setUnorderedAccessViewOffset(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(diffuseInputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(specularInputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(positionBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(normalRoughnessBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(albedoMetallicBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(materialIdBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(viewZInputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(motionInputBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(materialsBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(diffuseOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(specularOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(viewZOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(normalRoughnessOutputBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(motionOutputBinding));
            return desc;
        }

        nvrhi::BindingSetDesc makeRtDiNrdInputsBindingSetDesc(const RtDiNrdInputResources& inputs,
                                                              const RtDiNrdSignalResources& outputs,
                                                              nvrhi::IBuffer* constants) {
            if (!inputs.diffuseRadianceHitT || !inputs.specularRadianceHitT || !inputs.position ||
                !inputs.normalRoughness || !inputs.albedoMetallic || !inputs.materialId || !inputs.viewZ ||
                !inputs.motion || !inputs.materials || !outputs.diffuseRadianceHitDistance ||
                !outputs.specularRadianceHitDistance || !outputs.viewZ || !outputs.normalRoughness ||
                !outputs.motion || constants == nullptr) {
                throw std::invalid_argument("RTDI NRD preparation binding set requires complete resources.");
            }
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(diffuseInputBinding, inputs.diffuseRadianceHitT))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(specularInputBinding, inputs.specularRadianceHitT))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(positionBinding, inputs.position))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(normalRoughnessBinding, inputs.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(albedoMetallicBinding, inputs.albedoMetallic))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(materialIdBinding, inputs.materialId))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(viewZInputBinding, inputs.viewZ))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(motionInputBinding, inputs.motion))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(materialsBinding, inputs.materials))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, constants))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(diffuseOutputBinding,
                                                            outputs.diffuseRadianceHitDistance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(specularOutputBinding,
                                                            outputs.specularRadianceHitDistance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(viewZOutputBinding, outputs.viewZ))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(normalRoughnessOutputBinding, outputs.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(motionOutputBinding, outputs.motion));
            return desc;
        }

        std::uint32_t validateRtDiNrdInputResources(const RtDiNrdInputResources& inputs, core::RenderExtent extent) {
            if (extent.isEmpty()) {
                throw std::invalid_argument("RTDI NRD preparation requires a non-empty extent.");
            }
            validateTexture(inputs.diffuseRadianceHitT, extent, floatFourChannel, "raw diffuse radiance");
            validateTexture(inputs.specularRadianceHitT, extent, floatFourChannel, "raw specular radiance");
            validateTexture(inputs.position, extent, floatFourChannel, "primary position");
            validateTexture(inputs.normalRoughness, extent, floatFourChannel, "primary normal/roughness");
            validateTexture(inputs.albedoMetallic, extent, albedoFormat, "primary albedo/metallic");
            validateTexture(
                inputs.materialId, extent,
                [](nvrhi::Format format) {
                    return format == nvrhi::Format::R32_UINT;
                },
                "primary material ID");
            validateTexture(
                inputs.viewZ, extent,
                [](nvrhi::Format format) {
                    return format == nvrhi::Format::R16_FLOAT || format == nvrhi::Format::R32_FLOAT;
                },
                "primary view Z");
            validateTexture(
                inputs.motion, extent,
                [](nvrhi::Format format) {
                    return format == nvrhi::Format::RG16_FLOAT || format == nvrhi::Format::RG32_FLOAT;
                },
                "primary motion");
            if (!inputs.materials) {
                throw std::invalid_argument("RTDI NRD preparation is missing the material buffer.");
            }
            const nvrhi::BufferDesc& desc = inputs.materials->getDesc();
            if (desc.structStride != sizeof(gpu::GpuMaterialData) || desc.byteSize == 0 ||
                desc.byteSize % sizeof(gpu::GpuMaterialData) != 0) {
                throw std::invalid_argument("RTDI NRD material buffer does not use GpuMaterialData stride.");
            }
            const std::uint64_t materialCount = desc.byteSize / sizeof(gpu::GpuMaterialData);
            if (materialCount > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("RTDI NRD material table exceeds the shader index range.");
            }
            return static_cast<std::uint32_t>(materialCount);
        }

        RtDiNrdInputsConstants makeRtDiNrdInputsConstants(const RtDiNrdFrameParameters& parameters,
                                                          std::uint32_t materialCount) {
            if (!parameters.frameSlot.isValid() || parameters.extent.isEmpty() || materialCount == 0 ||
                !std::isfinite(parameters.cameraPosition.x) || !std::isfinite(parameters.cameraPosition.y) ||
                !std::isfinite(parameters.cameraPosition.z) || !std::isfinite(parameters.jitterDeltaUv.x) ||
                !std::isfinite(parameters.jitterDeltaUv.y) || !std::isfinite(parameters.denoisingRange) ||
                parameters.denoisingRange <= 0.0F) {
                throw std::invalid_argument("RTDI NRD constants require finite camera, jitter, range, and extent.");
            }
            RtDiNrdInputsConstants result;
            result.cameraPosition = glm::vec4{parameters.cameraPosition, 1.0F};
            result.renderParameters =
                glm::vec4{parameters.jitterDeltaUv, parameters.denoisingRange, 0.0F};
            result.renderInfo = glm::uvec4{parameters.extent.width, parameters.extent.height, materialCount,
                                           std::numeric_limits<std::uint32_t>::max()};
            return result;
        }

        RtDiNrdDispatchSize makeRtDiNrdDispatchSize(core::RenderExtent extent) {
            if (extent.isEmpty()) {
                throw std::invalid_argument("RTDI NRD dispatch requires a non-empty extent.");
            }
            const auto divideRoundUp = [](std::uint32_t value, std::uint32_t divisor) {
                return value / divisor + (value % divisor == 0 ? 0U : 1U);
            };
            return {divideRoundUp(extent.width, threadGroupWidth), divideRoundUp(extent.height, threadGroupHeight), 1};
        }

    } // namespace detail

    struct RtDiNrdInputsPass::Impl {
        nvrhi::IDevice& device;
        core::RenderExtent extent;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::ComputePipelineHandle pipeline;
        std::vector<RtDiNrdSignalResources> outputs;
        std::vector<nvrhi::BufferHandle> constants;
        std::vector<nvrhi::BindingSetHandle> bindingSets;
        std::vector<std::uint8_t> initialized;
        std::optional<std::uint32_t> pendingFrame;

        explicit Impl(const RtDiNrdInputsCreateInfo& createInfo)
            : device(*createInfo.device), extent(createInfo.extent), outputs(createInfo.frameSlotCount),
              constants(createInfo.frameSlotCount), bindingSets(createInfo.frameSlotCount),
              initialized(createInfo.frameSlotCount, 0) {
            bindingLayout = device.createBindingLayout(detail::makeRtDiNrdInputsBindingLayoutDesc());
            if (!bindingLayout) {
                throw std::runtime_error("Failed to create RTDI NRD preparation binding layout.");
            }
            PipelineFactory pipelines(device);
            const nvrhi::ShaderHandle compute = createInfo.shaders->load(ShaderId::RtDiNrdInputsCompute);
            const std::array layouts = {bindingLayout};
            pipeline = pipelines.createComputePipeline({compute, layouts});

            const RtDiNrdSignalFormats formats;
            for (std::uint32_t frameSlot = 0; frameSlot < createInfo.frameSlotCount; ++frameSlot) {
                const auto createTexture = [&](nvrhi::Format format, const char* debugName) {
                    nvrhi::TextureHandle texture = device.createTexture(detail::makeRtDiNrdSignalTextureDesc(
                        extent.width, extent.height, format, debugName));
                    if (!texture) {
                        throw std::runtime_error("Failed to create RTDI NRD preparation signal texture.");
                    }
                    return texture;
                };
                outputs[frameSlot] = RtDiNrdSignalResources{
                    .diffuseRadianceHitDistance = createTexture(formats.diffuseRadianceHitDistance,
                                                                "RTDI NRD diffuse radiance hit distance"),
                    .specularRadianceHitDistance = createTexture(formats.specularRadianceHitDistance,
                                                                 "RTDI NRD specular radiance hit distance"),
                    .viewZ = createTexture(formats.viewZ, "RTDI NRD view Z"),
                    .normalRoughness = createTexture(formats.normalRoughness, "RTDI NRD normal roughness"),
                    .motion = createTexture(formats.motion, "RTDI NRD motion"),
                };
                constants[frameSlot] = device.createBuffer(makeConstantBufferDesc());
                if (!constants[frameSlot]) {
                    throw std::runtime_error("Failed to create RTDI NRD preparation constant buffer.");
                }
            }
        }
    };

    RtDiNrdInputsPass::RtDiNrdInputsPass(const RtDiNrdInputsCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaders == nullptr || createInfo.extent.isEmpty() ||
            createInfo.frameSlotCount == 0) {
            throw std::invalid_argument("RTDI NRD preparation requires device, shaders, extent, and frame slots.");
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    RtDiNrdInputsPass::~RtDiNrdInputsPass() = default;

    RtDiNrdGraphOutput RtDiNrdInputsPass::record(FrameGraph& frameGraph,
                                                 const RtDiNrdFrameParameters& parameters,
                                                 const RtDiNrdInputResources& inputs,
                                                 const RtDiNrdInputGraphResources& graphInputs) {
        if (!parameters.frameSlotFenceWaited) {
            throw std::logic_error("RTDI NRD preparation may only update a frame slot after its fence wait.");
        }
        if (impl_->pendingFrame) {
            throw std::logic_error("RTDI NRD preparation already owns a pending frame.");
        }
        if (!parameters.frameSlot.isValid() || parameters.frameSlot.value() >= impl_->outputs.size()) {
            throw std::out_of_range("RTDI NRD preparation frame slot is outside the configured range.");
        }
        if (parameters.extent != impl_->extent || !complete(graphInputs)) {
            throw std::invalid_argument("RTDI NRD preparation requires matching extent and graph resources.");
        }
        const std::uint32_t materialCount = detail::validateRtDiNrdInputResources(inputs, parameters.extent);
        const RtDiNrdInputsConstants constants = detail::makeRtDiNrdInputsConstants(parameters, materialCount);
        const std::uint32_t frameSlot = parameters.frameSlot.value();
        writeConstants(impl_->device, impl_->constants[frameSlot], constants);

        nvrhi::BindingSetHandle bindingSet = impl_->device.createBindingSet(
            detail::makeRtDiNrdInputsBindingSetDesc(inputs, impl_->outputs[frameSlot], impl_->constants[frameSlot]),
            impl_->bindingLayout);
        if (!bindingSet) {
            throw std::runtime_error("Failed to create RTDI NRD preparation binding set.");
        }
        impl_->bindingSets[frameSlot] = bindingSet;

        const auto importOutput = [&](const char* name, nvrhi::ITexture* texture) {
            return frameGraph.importTexture(
                std::string{name} + "." + std::to_string(frameSlot),
                FrameGraphTextureDesc{.texture = texture,
                                      .initialState = impl_->initialized[frameSlot] != 0
                                                          ? nvrhi::ResourceStates::ShaderResource
                                                          : nvrhi::ResourceStates::Common,
                                      .finalState = nvrhi::ResourceStates::ShaderResource});
        };
        const RtDiNrdSignalResources& nativeOutputs = impl_->outputs[frameSlot];
        RtDiNrdGraphOutput output{
            .diffuseRadianceHitDistance =
                importOutput("rt-di-nrd.diffuse", nativeOutputs.diffuseRadianceHitDistance),
            .specularRadianceHitDistance =
                importOutput("rt-di-nrd.specular", nativeOutputs.specularRadianceHitDistance),
            .viewZ = importOutput("rt-di-nrd.view-z", nativeOutputs.viewZ),
            .normalRoughness = importOutput("rt-di-nrd.normal-roughness", nativeOutputs.normalRoughness),
            .motion = importOutput("rt-di-nrd.motion", nativeOutputs.motion),
            .readyPass = {},
        };
        const FrameGraphResourceHandle constantsResource = frameGraph.importBuffer(
            "rt-di-nrd.constants." + std::to_string(frameSlot),
            FrameGraphBufferDesc{.size = sizeof(RtDiNrdInputsConstants),
                                 .buffer = impl_->constants[frameSlot],
                                 .initialState = nvrhi::ResourceStates::ConstantBuffer,
                                 .finalState = nvrhi::ResourceStates::ConstantBuffer});
        const RtDiNrdDispatchSize dispatch = detail::makeRtDiNrdDispatchSize(parameters.extent);
        const nvrhi::ComputePipelineHandle pipeline = impl_->pipeline;
        output.readyPass = frameGraph.addPass(
            "rt-di-nrd-prepare", FrameGraphPassType::Compute,
            [graphInputs, output, constantsResource](FrameGraphBuilder& builder) {
                if (graphInputs.readyPass.isValid()) {
                    builder.dependsOn(graphInputs.readyPass);
                }
                builder.readTexture(graphInputs.diffuseRadianceHitT, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphInputs.specularRadianceHitT, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphInputs.position, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphInputs.normalRoughness, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphInputs.albedoMetallic, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphInputs.materialId, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphInputs.viewZ, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphInputs.motion, nvrhi::ResourceStates::ShaderResource);
                builder.read(graphInputs.materials, nvrhi::ResourceStates::ShaderResource);
                builder.read(constantsResource, nvrhi::ResourceStates::ConstantBuffer);
                builder.writeTexture(output.diffuseRadianceHitDistance, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.specularRadianceHitDistance, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.viewZ, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.normalRoughness, nvrhi::ResourceStates::UnorderedAccess);
                builder.writeTexture(output.motion, nvrhi::ResourceStates::UnorderedAccess);
            },
            [pipeline, bindingSet, dispatch](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("RTDI NRD preparation requires an NvRHI command list.");
                }
                nvrhi::ComputeState state;
                state.setPipeline(pipeline).addBindingSet(bindingSet);
                detail::recordRtDiNrdDispatch(*context.commandList, state, dispatch);
            });
        impl_->pendingFrame = frameSlot;
        return output;
    }

    void RtDiNrdInputsPass::commitSubmittedFrame() {
        if (impl_->pendingFrame) {
            impl_->initialized[*impl_->pendingFrame] = 1;
            impl_->pendingFrame.reset();
        }
    }

    void RtDiNrdInputsPass::discardPendingFrame() noexcept {
        impl_->pendingFrame.reset();
    }

    bool RtDiNrdInputsPass::hasPendingFrame() const noexcept {
        return impl_->pendingFrame.has_value();
    }

    const RtDiNrdSignalResources& RtDiNrdInputsPass::resources(std::uint32_t frameSlot) const {
        if (frameSlot >= impl_->outputs.size()) {
            throw std::out_of_range("RTDI NRD signal frame slot is outside the configured range.");
        }
        return impl_->outputs[frameSlot];
    }

    RtDiNrdSignalFormats RtDiNrdInputsPass::formats() const noexcept {
        return {};
    }

} // namespace lumin::render::gi

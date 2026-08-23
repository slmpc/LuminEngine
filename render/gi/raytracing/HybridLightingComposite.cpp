#include "render/gi/raytracing/HybridLightingComposite.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lumin::render::gi {
    namespace {

        constexpr std::uint32_t directBinding = 0;
        constexpr std::uint32_t indirectBinding = 1;
        constexpr std::uint32_t outputBinding = 2;
        constexpr std::uint32_t constantsBinding = 3;
        constexpr std::uint32_t threadGroupWidth = 8;
        constexpr std::uint32_t threadGroupHeight = 8;

        [[nodiscard]] nvrhi::BufferHandle createConstants(nvrhi::IDevice& device) {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(HybridLightingCompositeConstants);
            desc.debugName = "Hybrid lighting composite constants";
            desc.isConstantBuffer = true;
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
            nvrhi::BufferHandle buffer = device.createBuffer(desc);
            if (!buffer) {
                throw std::runtime_error("Failed to create Hybrid lighting composite constants.");
            }
            return buffer;
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer,
                            const HybridLightingCompositeConstants& constants) {
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map Hybrid lighting composite constants.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

        [[nodiscard]] std::uint32_t divideRoundUp(std::uint32_t value, std::uint32_t divisor) {
            return value / divisor + (value % divisor == 0 ? 0U : 1U);
        }

        [[nodiscard]] bool validTexture(nvrhi::ITexture* texture, core::RenderExtent extent, bool requireUav) {
            if (texture == nullptr) {
                return false;
            }
            const nvrhi::TextureDesc& desc = texture->getDesc();
            return desc.width == extent.width && desc.height == extent.height &&
                   desc.dimension == nvrhi::TextureDimension::Texture2D && desc.isShaderResource &&
                   (desc.format == nvrhi::Format::RGBA16_FLOAT || desc.format == nvrhi::Format::RGBA32_FLOAT) &&
                   (!requireUav || desc.isUAV);
        }

    } // namespace

    struct HybridLightingCompositePass::Impl {
        nvrhi::IDevice& device;
        core::RenderExtent extent;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::ComputePipelineHandle pipeline;
        std::vector<nvrhi::BufferHandle> constants;

        explicit Impl(const HybridLightingCompositeCreateInfo& createInfo)
            : device(*createInfo.device), extent(createInfo.extent), constants(createInfo.frameSlotCount) {
            PipelineFactory pipelines(device);
            bindingLayout =
                device.createBindingLayout(nvrhi::BindingLayoutDesc()
                                               .setVisibility(nvrhi::ShaderType::Compute)
                                               .setRegisterSpaceAndDescriptorSet(0)
                                               .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                                                      .setShaderResourceOffset(0)
                                                                      .setSamplerOffset(0)
                                                                      .setConstantBufferOffset(0)
                                                                      .setUnorderedAccessViewOffset(0))
                                               .addItem(nvrhi::BindingLayoutItem::Texture_SRV(directBinding))
                                               .addItem(nvrhi::BindingLayoutItem::Texture_SRV(indirectBinding))
                                               .addItem(nvrhi::BindingLayoutItem::Texture_UAV(outputBinding))
                                               .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding)));
            if (!bindingLayout) {
                throw std::runtime_error("Failed to create Hybrid lighting composite binding layout.");
            }
            const nvrhi::ShaderHandle shader = createInfo.shaders->load(ShaderId::HybridLightingCompositeCompute);
            const std::array layouts = {bindingLayout};
            pipeline = pipelines.createComputePipeline(
                ComputePipelineDesc{.computeShader = shader, .bindingLayouts = layouts});
            for (nvrhi::BufferHandle& buffer : constants) {
                buffer = createConstants(device);
            }
        }
    };

    HybridLightingCompositePass::HybridLightingCompositePass(const HybridLightingCompositeCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaders == nullptr || createInfo.extent.isEmpty() ||
            createInfo.frameSlotCount == 0) {
            throw std::invalid_argument(
                "Hybrid lighting composite requires device, extent, shader library, and slots.");
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    HybridLightingCompositePass::~HybridLightingCompositePass() = default;

    FrameGraphPassHandle HybridLightingCompositePass::record(
        FrameGraph& frameGraph, const HybridLightingCompositeFrameParameters& parameters,
        const HybridLightingCompositeResources& resources, const HybridLightingCompositeGraphResources& graphResources,
        FrameGraphPassHandle dependency) {
        if (!parameters.frameSlot.isValid() || parameters.frameSlot.value() >= impl_->constants.size()) {
            throw std::out_of_range("Hybrid lighting composite frame slot is out of range.");
        }
        if (!parameters.frameSlotFenceWaited || parameters.extent != impl_->extent || !graphResources.isValid() ||
            !validTexture(resources.directRadiance, parameters.extent, false) ||
            !validTexture(resources.indirectRadiance, parameters.extent, false) ||
            !validTexture(resources.output, parameters.extent, true)) {
            throw std::invalid_argument("Hybrid lighting composite resource contract is invalid.");
        }
        switch (parameters.mode) {
        case HybridLightingCompositeMode::DirectAndIndirect:
        case HybridLightingCompositeMode::DirectOnly:
        case HybridLightingCompositeMode::DirectWithAmbientVisibility:
            break;
        default:
            throw std::invalid_argument("Hybrid lighting composite mode is invalid.");
        }

        const std::uint32_t frameSlot = parameters.frameSlot.value();
        writeConstants(impl_->device, impl_->constants[frameSlot],
                       HybridLightingCompositeConstants{{parameters.extent.width, parameters.extent.height,
                                                         static_cast<std::uint32_t>(parameters.mode), 0U}});
        const nvrhi::BindingSetHandle bindingSet = impl_->device.createBindingSet(
            nvrhi::BindingSetDesc()
                .addItem(nvrhi::BindingSetItem::Texture_SRV(directBinding, resources.directRadiance))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(indirectBinding, resources.indirectRadiance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(outputBinding, resources.output))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, impl_->constants[frameSlot])),
            impl_->bindingLayout);
        if (!bindingSet) {
            throw std::runtime_error("Failed to create Hybrid lighting composite binding set.");
        }

        const FrameGraphResourceHandle constantsResource =
            frameGraph.importBuffer("hybrid-lighting-composite.constants." + std::to_string(frameSlot),
                                    FrameGraphBufferDesc{.size = sizeof(HybridLightingCompositeConstants),
                                                         .buffer = impl_->constants[frameSlot],
                                                         .initialState = nvrhi::ResourceStates::ConstantBuffer,
                                                         .finalState = nvrhi::ResourceStates::ConstantBuffer});
        const nvrhi::ComputePipelineHandle pipeline = impl_->pipeline;
        const std::uint32_t dispatchX = divideRoundUp(parameters.extent.width, threadGroupWidth);
        const std::uint32_t dispatchY = divideRoundUp(parameters.extent.height, threadGroupHeight);
        return frameGraph.addPass(
            "Hybrid lighting composite", FrameGraphPassType::Compute,
            [graphResources, constantsResource, dependency](FrameGraphBuilder& builder) {
                if (dependency.isValid()) {
                    builder.dependsOn(dependency);
                }
                builder.readTexture(graphResources.directRadiance, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(graphResources.indirectRadiance, nvrhi::ResourceStates::ShaderResource);
                builder.read(constantsResource, nvrhi::ResourceStates::ConstantBuffer);
                builder.writeTexture(graphResources.output, nvrhi::ResourceStates::UnorderedAccess);
            },
            [pipeline, bindingSet, dispatchX, dispatchY](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("Hybrid lighting composite requires a command list.");
                }
                nvrhi::ComputeState state;
                state.setPipeline(pipeline).addBindingSet(bindingSet);
                context.commandList->setComputeState(state);
                context.commandList->dispatch(dispatchX, dispatchY, 1);
            });
    }

} // namespace lumin::render::gi

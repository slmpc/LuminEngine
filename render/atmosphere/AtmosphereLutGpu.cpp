#include "render/atmosphere/AtmosphereLutGpu.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lumin::render::atmosphere {
    namespace {

        constexpr std::uint32_t constantsBinding = 0;
        constexpr std::uint32_t transmittanceBinding = 1;
        constexpr std::uint32_t multiScatteringBinding = 2;
        constexpr std::uint32_t samplerBinding = 3;
        constexpr std::uint32_t outputBinding = 4;
        constexpr std::uint32_t consumerSkyViewBinding = 3;
        constexpr std::uint32_t consumerAerialPerspectiveBinding = 4;
        constexpr std::uint32_t consumerSamplerBinding = 5;

        [[nodiscard]] std::string_view lutDebugName(AtmosphereLut lut) {
            switch (lut) {
            case AtmosphereLut::Transmittance:
                return "Atmosphere transmittance LUT";
            case AtmosphereLut::MultiScattering:
                return "Atmosphere multi-scattering LUT";
            case AtmosphereLut::SkyView:
                return "Atmosphere sky-view LUT";
            case AtmosphereLut::AerialPerspective:
                return "Atmosphere aerial-perspective LUT";
            case AtmosphereLut::Count:
                break;
            }
            throw std::invalid_argument("Atmosphere LUT is invalid.");
        }

        struct AtmosphereLutShaderInfo {
            const char* fileName = nullptr;
            const char* entryPoint = nullptr;
        };

        [[nodiscard]] AtmosphereLutShaderInfo shaderInfo(AtmosphereLut lut) {
            switch (lut) {
            case AtmosphereLut::Transmittance:
                return {"TransmittanceLut.comp.spv", "transmittanceMain"};
            case AtmosphereLut::MultiScattering:
                return {"MultiScatteringLut.comp.spv", "multiScatteringMain"};
            case AtmosphereLut::SkyView:
                return {"SkyViewLut.comp.spv", "skyViewMain"};
            case AtmosphereLut::AerialPerspective:
                return {"AerialPerspectiveLut.comp.spv", "aerialPerspectiveMain"};
            case AtmosphereLut::Count:
                break;
            }
            throw std::invalid_argument("Atmosphere LUT is invalid.");
        }

        [[nodiscard]] bool planRebuildsEveryLut(const AtmosphereLutPassPlan& plan) {
            std::array<bool, atmosphereLutResourceCount> present{};
            for (const AtmosphereLutComputePass& pass : plan.passes()) {
                present[atmosphereLutResourceIndex(pass.target())] = true;
            }
            return std::ranges::all_of(present, [](bool value) {
                return value;
            });
        }

        [[nodiscard]] std::uint32_t divideRoundUp(std::uint32_t value, std::uint32_t divisor) {
            if (value == 0 || divisor == 0) {
                throw std::invalid_argument("Atmosphere LUT dispatch inputs must be non-zero.");
            }
            return value / divisor + (value % divisor == 0 ? 0U : 1U);
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer, const AtmosphereGpuConstants& constants) {
            if (buffer == nullptr) {
                throw std::invalid_argument("Atmosphere LUT constants buffer is null.");
            }
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map atmosphere LUT constants buffer.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

    } // namespace

    const nvrhi::TextureHandle& AtmosphereLutNativeResources::texture(AtmosphereLut lut) const {
        return textures[atmosphereLutResourceIndex(lut)];
    }

    bool AtmosphereLutNativeResources::isValid() const noexcept {
        return sampler && std::ranges::all_of(textures, [](const nvrhi::TextureHandle& texture) {
                   return static_cast<bool>(texture);
               });
    }

    namespace detail {

        nvrhi::TextureDesc makeAtmosphereLutTextureDesc(const AtmosphereLutResourceDesc& resource) {
            if (!validateAtmosphereLutResourceDesc(resource)) {
                throw std::invalid_argument("Atmosphere LUT texture description violates the resource contract.");
            }

            nvrhi::TextureDesc desc;
            desc.width = resource.extent.width;
            desc.height = resource.extent.height;
            desc.depth = resource.extent.depth;
            desc.arraySize = 1;
            desc.mipLevels = 1;
            desc.sampleCount = 1;
            desc.format = resource.format;
            desc.dimension = resource.dimension;
            desc.debugName = std::string{lutDebugName(resource.lut)};
            desc.isShaderResource = true;
            desc.isUAV = true;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            return desc;
        }

        nvrhi::BufferDesc makeAtmosphereLutConstantBufferDesc() {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(AtmosphereGpuConstants);
            desc.debugName = "Atmosphere LUT constants";
            desc.isConstantBuffer = true;
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            return desc;
        }

        nvrhi::SamplerDesc makeAtmosphereLutSamplerDesc() noexcept {
            nvrhi::SamplerDesc desc;
            desc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
            return desc;
        }

        nvrhi::BindingLayoutDesc makeAtmosphereLutBindingLayoutDesc(AtmosphereLut target) {
            static_cast<void>(atmosphereLutResourceIndex(target));
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                       .setShaderResourceOffset(0)
                                       .setSamplerOffset(0)
                                       .setConstantBufferOffset(0)
                                       .setUnorderedAccessViewOffset(0))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding));

            if (target != AtmosphereLut::Transmittance) {
                desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(transmittanceBinding));
            }
            if (target == AtmosphereLut::SkyView || target == AtmosphereLut::AerialPerspective) {
                desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(multiScatteringBinding));
            }
            if (target != AtmosphereLut::Transmittance) {
                desc.addItem(nvrhi::BindingLayoutItem::Sampler(samplerBinding));
            }
            desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(outputBinding));
            return desc;
        }

        nvrhi::BindingSetDesc makeAtmosphereLutBindingSetDesc(AtmosphereLut target, nvrhi::IBuffer* constants,
                                                              const AtmosphereLutNativeResources& resources) {
            static_cast<void>(atmosphereLutResourceIndex(target));
            if (constants == nullptr || !resources.isValid()) {
                throw std::invalid_argument("Atmosphere LUT binding set requires complete native resources.");
            }

            const nvrhi::TextureHandle& output = resources.texture(target);
            const nvrhi::TextureDimension expectedDimension = target == AtmosphereLut::AerialPerspective
                                                                  ? nvrhi::TextureDimension::Texture3D
                                                                  : nvrhi::TextureDimension::Texture2D;
            if (!output || output->getDesc().dimension != expectedDimension) {
                throw std::invalid_argument("Atmosphere LUT output texture dimension is incompatible with its pass.");
            }

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, constants));
            if (target != AtmosphereLut::Transmittance) {
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                    transmittanceBinding, resources.texture(AtmosphereLut::Transmittance), nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D));
            }
            if (target == AtmosphereLut::SkyView || target == AtmosphereLut::AerialPerspective) {
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                    multiScatteringBinding, resources.texture(AtmosphereLut::MultiScattering), nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D));
            }
            if (target != AtmosphereLut::Transmittance) {
                desc.addItem(nvrhi::BindingSetItem::Sampler(samplerBinding, resources.sampler));
            }
            desc.addItem(nvrhi::BindingSetItem::Texture_UAV(outputBinding, output, nvrhi::Format::UNKNOWN,
                                                            nvrhi::TextureSubresourceSet{0, 1, 0, 1},
                                                            expectedDimension));
            return desc;
        }

        nvrhi::BindingLayoutDesc makeAtmosphereConsumerBindingLayoutDesc() {
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::All)
                .setRegisterSpaceAndDescriptorSet(2)
                .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                       .setShaderResourceOffset(0)
                                       .setSamplerOffset(0)
                                       .setConstantBufferOffset(0)
                                       .setUnorderedAccessViewOffset(0))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(transmittanceBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(multiScatteringBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(consumerSkyViewBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(consumerAerialPerspectiveBinding))
                .addItem(nvrhi::BindingLayoutItem::Sampler(consumerSamplerBinding));
            return desc;
        }

        nvrhi::BindingSetDesc makeAtmosphereConsumerBindingSetDesc(nvrhi::IBuffer* constants,
                                                                   const AtmosphereLutNativeResources& resources) {
            if (constants == nullptr || !resources.isValid()) {
                throw std::invalid_argument("Atmosphere consumer bindings require complete native resources.");
            }
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, constants))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    transmittanceBinding, resources.texture(AtmosphereLut::Transmittance), nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    multiScatteringBinding, resources.texture(AtmosphereLut::MultiScattering), nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    consumerSkyViewBinding, resources.texture(AtmosphereLut::SkyView), nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    consumerAerialPerspectiveBinding, resources.texture(AtmosphereLut::AerialPerspective),
                    nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture3D))
                .addItem(nvrhi::BindingSetItem::Sampler(consumerSamplerBinding, resources.sampler));
            return desc;
        }

        AtmosphereLutThreadGroupSize atmosphereLutThreadGroupSize(AtmosphereLut target) {
            switch (target) {
            case AtmosphereLut::Transmittance:
            case AtmosphereLut::MultiScattering:
            case AtmosphereLut::SkyView:
                return {8, 8, 1};
            case AtmosphereLut::AerialPerspective:
                return {4, 4, 4};
            case AtmosphereLut::Count:
                break;
            }
            throw std::invalid_argument("Atmosphere LUT is invalid.");
        }

        AtmosphereLutDispatchSize makeAtmosphereLutDispatchSize(const AtmosphereLutResourceDesc& resource) {
            if (!validateAtmosphereLutResourceDesc(resource)) {
                throw std::invalid_argument("Atmosphere LUT dispatch requires a valid resource description.");
            }
            const AtmosphereLutThreadGroupSize group = atmosphereLutThreadGroupSize(resource.lut);
            return {
                divideRoundUp(resource.extent.width, group.x),
                divideRoundUp(resource.extent.height, group.y),
                divideRoundUp(resource.extent.depth, group.z),
            };
        }

    } // namespace detail

    struct AtmosphereLutGpu::Impl {
        struct PendingFrame {
            std::uint32_t frameSlot = 0;
        };

        nvrhi::IDevice& device;
        AtmosphereLutQuality quality;
        AtmosphereLutResourceSet resourceDescs;
        AtmosphereLutNativeResources resources;
        std::vector<nvrhi::BufferHandle> constants;
        std::vector<std::array<nvrhi::BindingSetHandle, atmosphereLutResourceCount>> bindingSets;
        std::array<nvrhi::BindingLayoutHandle, atmosphereLutResourceCount> bindingLayouts{};
        std::array<nvrhi::ComputePipelineHandle, atmosphereLutResourceCount> pipelines{};
        std::vector<std::uint8_t> constantsInitialized;
        bool texturesInitialized = false;
        std::optional<PendingFrame> pending;

        explicit Impl(const AtmosphereLutGpuCreateInfo& createInfo)
            : device(*createInfo.device), quality(createInfo.quality),
              resourceDescs(makeAtmosphereLutResourceSet(quality)), constantsInitialized(createInfo.frameSlotCount, 0) {
            for (const AtmosphereLutResourceDesc& resource : resourceDescs) {
                if (!supportsAtmosphereLutFormat(resource, device.queryFormatSupport(resource.format))) {
                    throw std::runtime_error(
                        "Device does not support RGBA16_FLOAT atmosphere LUT sampling and UAV stores.");
                }
                nvrhi::TextureHandle texture = device.createTexture(detail::makeAtmosphereLutTextureDesc(resource));
                if (!texture) {
                    throw std::runtime_error("Failed to create an atmosphere LUT texture.");
                }
                resources.textures[atmosphereLutResourceIndex(resource.lut)] = std::move(texture);
            }
            resources.sampler = device.createSampler(detail::makeAtmosphereLutSamplerDesc());
            if (!resources.sampler) {
                throw std::runtime_error("Failed to create the atmosphere LUT sampler.");
            }

            ShaderLibrary shaders(device, createInfo.shaderDirectory);
            PipelineFactory pipelineFactory(device);
            for (std::size_t index = 0; index < atmosphereLutResourceCount; ++index) {
                const AtmosphereLut target = static_cast<AtmosphereLut>(index);
                bindingLayouts[index] = device.createBindingLayout(detail::makeAtmosphereLutBindingLayoutDesc(target));
                if (!bindingLayouts[index]) {
                    throw std::runtime_error("Failed to create an atmosphere LUT binding layout.");
                }
                const AtmosphereLutShaderInfo shader = shaderInfo(target);
                const nvrhi::ShaderHandle compute = shaders.loadComputeModule(shader.fileName, shader.entryPoint);
                const std::array layouts = {bindingLayouts[index]};
                pipelines[index] = pipelineFactory.createComputePipeline({compute, layouts});
            }

            constants.reserve(createInfo.frameSlotCount);
            bindingSets.resize(createInfo.frameSlotCount);
            for (std::uint32_t frameSlot = 0; frameSlot < createInfo.frameSlotCount; ++frameSlot) {
                nvrhi::BufferHandle buffer = device.createBuffer(detail::makeAtmosphereLutConstantBufferDesc());
                if (!buffer) {
                    throw std::runtime_error("Failed to create an atmosphere LUT constants buffer.");
                }
                constants.push_back(std::move(buffer));
                for (std::size_t index = 0; index < atmosphereLutResourceCount; ++index) {
                    const AtmosphereLut target = static_cast<AtmosphereLut>(index);
                    bindingSets[frameSlot][index] = device.createBindingSet(
                        detail::makeAtmosphereLutBindingSetDesc(target, constants[frameSlot], resources),
                        bindingLayouts[index]);
                    if (!bindingSets[frameSlot][index]) {
                        throw std::runtime_error("Failed to create an atmosphere LUT binding set.");
                    }
                }
            }
        }
    };

    AtmosphereLutGpu::AtmosphereLutGpu(const AtmosphereLutGpuCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaderDirectory.empty() || createInfo.frameSlotCount == 0 ||
            !validateAtmosphereLutQuality(createInfo.quality)) {
            throw std::invalid_argument(
                "Atmosphere LUT GPU owner requires a device, shader directory, frame slots, and valid quality.");
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    AtmosphereLutGpu::~AtmosphereLutGpu() = default;

    AtmosphereLutGraphRecord AtmosphereLutGpu::record(FrameGraph& frameGraph, std::uint32_t frameSlot,
                                                      bool frameSlotFenceWaited,
                                                      const AtmosphereGpuConstants& constants,
                                                      const AtmosphereLutPassPlan& plan) {
        if (!plan.isValid()) {
            throw std::invalid_argument("Atmosphere LUT GPU record requires a valid pass plan.");
        }
        if (frameSlot >= impl_->constants.size()) {
            throw std::out_of_range("Atmosphere LUT frame slot is outside the configured range.");
        }
        if (!frameSlotFenceWaited) {
            throw std::logic_error("Atmosphere LUT constants may only change after waiting for the slot fence.");
        }
        if (impl_->pending) {
            throw std::logic_error("Atmosphere LUT GPU owner already has a pending frame.");
        }
        if (!impl_->texturesInitialized && !planRebuildsEveryLut(plan)) {
            throw std::logic_error("The first atmosphere LUT submission must rebuild all LUT resources.");
        }

        // LUT 即使全部复用，raster/RT/GI consumer 仍会读取当前帧槽常量；每个槽首次使用时也必须初始化。
        writeConstants(impl_->device, impl_->constants[frameSlot], constants);

        AtmosphereLutGraphRecord result;
        for (const AtmosphereLutResourceDesc& resource : impl_->resourceDescs) {
            const std::size_t index = atmosphereLutResourceIndex(resource.lut);
            result.resources.textures[index] = frameGraph.importTexture(
                std::string{lutDebugName(resource.lut)},
                FrameGraphTextureDesc{
                    .texture = impl_->resources.textures[index],
                    .initialState = impl_->texturesInitialized ? nvrhi::ResourceStates::ShaderResource
                                                               : nvrhi::ResourceStates::Common,
                    .finalState = nvrhi::ResourceStates::ShaderResource,
                });
        }
        result.resources.constants = frameGraph.importBuffer(
            "Atmosphere LUT constants " + std::to_string(frameSlot),
            FrameGraphBufferDesc{
                .size = sizeof(AtmosphereGpuConstants),
                .buffer = impl_->constants[frameSlot],
                .initialState = impl_->constantsInitialized[frameSlot] != 0 ? nvrhi::ResourceStates::ConstantBuffer
                                                                            : nvrhi::ResourceStates::Common,
                .finalState = nvrhi::ResourceStates::ConstantBuffer,
            });

        AtmosphereLutExecuteCallbacks callbacks;
        for (const AtmosphereLutComputePass& pass : plan.passes()) {
            const std::size_t index = atmosphereLutResourceIndex(pass.target());
            const nvrhi::ComputePipelineHandle pipeline = impl_->pipelines[index];
            const nvrhi::BindingSetHandle bindingSet = impl_->bindingSets[frameSlot][index];
            const AtmosphereLutDispatchSize dispatch =
                detail::makeAtmosphereLutDispatchSize(impl_->resourceDescs[index]);
            callbacks.callbacks[index] = [pipeline, bindingSet, dispatch](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("Atmosphere LUT dispatch requires an NvRHI command list.");
                }
                nvrhi::ComputeState state;
                state.setPipeline(pipeline).addBindingSet(bindingSet);
                detail::recordAtmosphereLutDispatch(*context.commandList, state, dispatch);
            };
        }
        result.passes = registerAtmosphereLutPasses(frameGraph, plan, result.resources, std::move(callbacks));
        impl_->pending = Impl::PendingFrame{frameSlot};
        return result;
    }

    void AtmosphereLutGpu::commitSubmittedFrame() noexcept {
        if (!impl_->pending) {
            return;
        }
        impl_->texturesInitialized = true;
        impl_->constantsInitialized[impl_->pending->frameSlot] = 1;
        impl_->pending.reset();
    }

    void AtmosphereLutGpu::discardPendingFrame() noexcept {
        impl_->pending.reset();
    }

    bool AtmosphereLutGpu::hasPendingFrame() const noexcept {
        return impl_->pending.has_value();
    }

    const AtmosphereLutNativeResources& AtmosphereLutGpu::nativeResources() const noexcept {
        return impl_->resources;
    }

    const nvrhi::BufferHandle& AtmosphereLutGpu::constantBuffer(std::uint32_t frameSlot) const {
        if (frameSlot >= impl_->constants.size()) {
            throw std::out_of_range("Atmosphere LUT frame slot is outside the configured range.");
        }
        return impl_->constants[frameSlot];
    }

    const AtmosphereLutQuality& AtmosphereLutGpu::quality() const noexcept {
        return impl_->quality;
    }

} // namespace lumin::render::atmosphere

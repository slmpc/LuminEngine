#include "render/gi/legacy/LegacyBackend.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lumin::render::gi {
    namespace {

        constexpr std::uint32_t positionBinding = 0;
        constexpr std::uint32_t normalBinding = 1;
        constexpr std::uint32_t samplerBinding = 2;
        constexpr std::uint32_t uniformBinding = 3;

        class LegacyBackend final : public GlobalIlluminationBackend {
        public:
            LegacyBackend() = default;

            ~LegacyBackend() override {
                destroy();
            }

            [[nodiscard]] BackendInfo info() const noexcept override {
                return BackendInfo{"SSAO / HBAO / GTAO", false, false};
            }

            void create(const CreateInfo& createInfo) override {
                destroy();
                const bool hasTestDriver =
#if defined(LUMIN_GI_TESTING)
                    createInfo.creationDriver != nullptr;
#else
                    false;
#endif
                if ((!hasTestDriver && (createInfo.device == nullptr || createInfo.shaders == nullptr)) ||
                    createInfo.extent.width == 0 || createInfo.extent.height == 0 ||
                    createInfo.outputFormat == nvrhi::Format::UNKNOWN || !createInfo.sampler ||
                    createInfo.frames.empty()) {
                    throw std::invalid_argument("Legacy backend requires complete render resources.");
                }
                for (const FrameResources& frame : createInfo.frames) {
                    if (!frame.position || !frame.normalRoughness || (!hasTestDriver && !frame.uniformBuffer) ||
                        !frame.output) {
                        throw std::invalid_argument("Legacy backend received an incomplete frame resource.");
                    }
                }

                device_ = createInfo.device;
                shaders_ = createInfo.shaders;
                frames_.assign(createInfo.frames.begin(), createInfo.frames.end());
#if defined(LUMIN_GI_TESTING)
                creationDriver_ = createInfo.creationDriver;
                recordProbe_ = createInfo.recordProbe;
#endif
                if (!hasTestDriver) {
                    pipelineFactory_ = std::make_unique<PipelineFactory>(*device_);
                }

                try {
#if defined(LUMIN_GI_TESTING)
                    if (creationDriver_ != nullptr) {
                        for (FrameResources& frame : frames_) {
                            frame.uniformBuffer = creationDriver_->createUniform();
                            if (!frame.uniformBuffer) {
                                throw std::runtime_error("Failed to create a Legacy AO uniform buffer.");
                            }
                        }
                    }
#endif
                    createBindings(createInfo.sampler);
                    createFramebuffers();
                    createPipeline(createInfo.outputFormat);
                } catch (...) {
                    destroy();
                    throw;
                }
            }

            void destroy() noexcept override {
                pipeline_.Reset();
                framebuffers_.clear();
                bindingSets_.clear();
                bindingLayout_.Reset();
                frames_.clear();
                pipelineFactory_.reset();
                shaders_ = nullptr;
                device_ = nullptr;
#if defined(LUMIN_GI_TESTING)
                creationDriver_ = nullptr;
                recordProbe_ = nullptr;
#endif
            }

            void invalidateHistory() noexcept override {
            }

            void addPasses(FrameGraph& frameGraph, const FrameInfo& frameInfo) override {
                if (!pipeline_ || frameInfo.frameIndex >= frames_.size() ||
                    frameInfo.frameIndex >= bindingSets_.size() || frameInfo.frameIndex >= framebuffers_.size()) {
                    throw std::logic_error("Legacy backend is not ready for the requested frame slot.");
                }
                if (frameInfo.extent.width == 0 || frameInfo.extent.height == 0) {
                    throw std::invalid_argument("Legacy AO pass requires a non-empty render extent.");
                }

                const nvrhi::TextureHandle output = frames_[frameInfo.frameIndex].output;
                const nvrhi::BindingSetHandle bindingSet = bindingSets_[frameInfo.frameIndex];
                const nvrhi::FramebufferHandle framebuffer = framebuffers_[frameInfo.frameIndex];
                const nvrhi::GraphicsPipelineHandle pipeline = pipeline_;
                const RenderExtent extent = frameInfo.extent;
#if defined(LUMIN_GI_TESTING)
                LegacyRecordProbe* const recordProbe = recordProbe_;
#endif

                frameGraph.addPass(
                    "GI: screen-space AO clear", FrameGraphPassType::Transfer,
                    [outputHandle = frameInfo.output](FrameGraphBuilder& builder) {
                        builder.writeTexture(outputHandle, nvrhi::ResourceStates::CopyDest);
                    },
                    [output
#if defined(LUMIN_GI_TESTING)
                     ,
                     recordProbe
#endif
                ](const FrameGraphContext& context) {
#if defined(LUMIN_GI_TESTING)
                        if (recordProbe != nullptr) {
                            detail::recordLegacyClear(*recordProbe, output);
                            return;
                        }
#endif
                        if (context.commandList == nullptr) {
                            throw std::logic_error("Legacy AO clear pass requires an NvRHI command list.");
                        }
                        detail::recordLegacyClear(*context.commandList, output);
                    });

                if (!frameInfo.enabled) {
                    return;
                }

                frameGraph.addPass(
                    "GI: screen-space AO", FrameGraphPassType::Graphics,
                    [position = frameInfo.position, normal = frameInfo.normalRoughness,
                     outputHandle = frameInfo.output](FrameGraphBuilder& builder) {
                        builder.readTexture(position, nvrhi::ResourceStates::ShaderResource);
                        builder.readTexture(normal, nvrhi::ResourceStates::ShaderResource);
                        builder.writeTexture(outputHandle, nvrhi::ResourceStates::RenderTarget);
                    },
                    [output, bindingSet, framebuffer, pipeline, extent
#if defined(LUMIN_GI_TESTING)
                     ,
                     recordProbe
#endif
                ](const FrameGraphContext& context) {
#if defined(LUMIN_GI_TESTING)
                        if (recordProbe != nullptr) {
                            nvrhi::GraphicsState state;
                            state.setPipeline(pipeline)
                                .setFramebuffer(framebuffer)
                                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(
                                    static_cast<float>(extent.width), static_cast<float>(extent.height))))
                                .addBindingSet(bindingSet);
                            detail::recordLegacyFullscreen(*recordProbe, state);
                            return;
                        }
#endif
                        if (context.commandList == nullptr) {
                            throw std::logic_error("Legacy AO pass requires an NvRHI command list.");
                        }
                        nvrhi::GraphicsState state;
                        state.setPipeline(pipeline)
                            .setFramebuffer(framebuffer)
                            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                                nvrhi::Viewport(static_cast<float>(extent.width), static_cast<float>(extent.height))))
                            .addBindingSet(bindingSet);
                        detail::recordLegacyFullscreen(*context.commandList, state);
                    });
            }

        private:
            void createBindings(const nvrhi::SamplerHandle& sampler) {
                nvrhi::VulkanBindingOffsets offsets;
                offsets.setShaderResourceOffset(0)
                    .setSamplerOffset(0)
                    .setConstantBufferOffset(0)
                    .setUnorderedAccessViewOffset(0);

                nvrhi::BindingLayoutDesc layoutDesc;
                layoutDesc.setVisibility(nvrhi::ShaderType::Pixel)
                    .setRegisterSpaceAndDescriptorSet(0)
                    .setBindingOffsets(offsets)
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(positionBinding))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(normalBinding))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(samplerBinding))
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(uniformBinding));
#if defined(LUMIN_GI_TESTING)
                if (creationDriver_ != nullptr) {
                    bindingLayout_ = creationDriver_->createBindingLayout(layoutDesc);
                } else
#endif
                {
                    bindingLayout_ = device_->createBindingLayout(layoutDesc);
                }
                if (!bindingLayout_) {
                    throw std::runtime_error("Failed to create the Legacy AO binding layout.");
                }

                bindingSets_.reserve(frames_.size());
                for (const FrameResources& frame : frames_) {
                    nvrhi::BindingSetDesc bindingDesc;
                    bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(positionBinding, frame.position))
                        .addItem(nvrhi::BindingSetItem::Texture_SRV(normalBinding, frame.normalRoughness))
                        .addItem(nvrhi::BindingSetItem::Sampler(samplerBinding, sampler))
                        .addItem(nvrhi::BindingSetItem::ConstantBuffer(uniformBinding, frame.uniformBuffer));
                    nvrhi::BindingSetHandle bindingSet;
#if defined(LUMIN_GI_TESTING)
                    if (creationDriver_ != nullptr) {
                        bindingSet = creationDriver_->createBindingSet(bindingDesc, bindingLayout_);
                    } else
#endif
                    {
                        bindingSet = device_->createBindingSet(bindingDesc, bindingLayout_);
                    }
                    if (!bindingSet) {
                        throw std::runtime_error("Failed to create a Legacy AO binding set.");
                    }
                    bindingSets_.push_back(std::move(bindingSet));
                }
            }

            void createFramebuffers() {
                framebuffers_.reserve(frames_.size());
                for (const FrameResources& frame : frames_) {
                    const nvrhi::FramebufferDesc desc = nvrhi::FramebufferDesc().addColorAttachment(frame.output);
#if defined(LUMIN_GI_TESTING)
                    if (creationDriver_ != nullptr) {
                        nvrhi::FramebufferHandle framebuffer = creationDriver_->createFramebuffer(desc);
                        if (!framebuffer) {
                            throw std::runtime_error("Failed to create a Legacy AO framebuffer.");
                        }
                        framebuffers_.push_back(std::move(framebuffer));
                        continue;
                    }
#endif
                    nvrhi::FramebufferHandle framebuffer = device_->createFramebuffer(desc);
                    if (!framebuffer) {
                        throw std::runtime_error("Failed to create a Legacy AO framebuffer.");
                    }
                    framebuffers_.push_back(std::move(framebuffer));
                }
            }

            void createPipeline(nvrhi::Format outputFormat) {
                const std::array<nvrhi::BindingLayoutHandle, 1> layouts = {bindingLayout_};
                const std::array<nvrhi::Format, 1> colorFormats = {outputFormat};
                GraphicsPipelineDesc desc;
                desc.bindingLayouts = layouts;
                desc.colorFormats = colorFormats;
                desc.depthTestEnable = false;
                desc.depthWriteEnable = false;
                desc.cullMode = nvrhi::RasterCullMode::None;
#if defined(LUMIN_GI_TESTING)
                if (creationDriver_ != nullptr) {
                    pipeline_ = creationDriver_->createPipeline(desc);
                    if (!pipeline_) {
                        throw std::runtime_error("Failed to create graphics pipeline.");
                    }
                    return;
                }
#endif
                const nvrhi::ShaderHandle vertexShader = shaders_->load(ShaderId::AmbientOcclusionVertex);
                const nvrhi::ShaderHandle fragmentShader = shaders_->load(ShaderId::AmbientOcclusionFragment);
                desc.vertexShader = vertexShader;
                desc.fragmentShader = fragmentShader;
                pipeline_ = pipelineFactory_->createGraphicsPipeline(desc);
            }

            nvrhi::IDevice* device_ = nullptr;
            std::vector<FrameResources> frames_;
            nvrhi::BindingLayoutHandle bindingLayout_;
            std::vector<nvrhi::BindingSetHandle> bindingSets_;
            std::vector<nvrhi::FramebufferHandle> framebuffers_;
            nvrhi::GraphicsPipelineHandle pipeline_;
            ShaderLibrary* shaders_ = nullptr;
            std::unique_ptr<PipelineFactory> pipelineFactory_;
#if defined(LUMIN_GI_TESTING)
            LegacyCreationDriver* creationDriver_ = nullptr;
            LegacyRecordProbe* recordProbe_ = nullptr;
#endif
        };

    } // namespace

    std::unique_ptr<GlobalIlluminationBackend> makeLegacyBackend() {
        return std::make_unique<LegacyBackend>();
    }

} // namespace lumin::render::gi

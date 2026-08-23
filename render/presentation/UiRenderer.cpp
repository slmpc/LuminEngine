#include "render/presentation/UiRenderer.hpp"

#include "render/resources/ShaderLibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <imgui.h>

namespace lumin::render {
    namespace {

        constexpr std::size_t minimumVertexCapacity = 5'000;
        constexpr std::size_t minimumIndexCapacity = 10'000;

        struct UiPushConstants {
            float scale[2];
            float translate[2];
            float outputConfig[4];
        };

    } // namespace

    UiRenderer::~UiRenderer() {
        shutdown();
    }

    void UiRenderer::initialize(const UiRendererConfig& config) {
        shutdown();
        if (config.device == nullptr || config.shaders == nullptr || config.colorFormat == nvrhi::Format::UNKNOWN ||
            config.frameSlotCount == 0 || config.fontAtlas == nullptr) {
            throw std::invalid_argument("Invalid UiRenderer configuration.");
        }
        device_ = config.device;
        shaders_ = config.shaders;
        outputIsSrgb_ = config.outputIsSrgb;
        try {
            createRendererResources(config);
            initialized_ = true;
        } catch (...) {
            shutdown();
            throw;
        }
    }

    void UiRenderer::shutdown() noexcept {
        textureBindings_.clear();
        frameBuffers_.clear();
        pipeline_ = nullptr;
        inputLayout_ = nullptr;
        vertexShader_ = nullptr;
        fragmentShader_ = nullptr;
        bindingLayout_ = nullptr;
        fontSampler_ = nullptr;
        fontTexture_ = nullptr;
        shaders_ = nullptr;
        device_ = nullptr;
        outputIsSrgb_ = false;
        initialized_ = false;
    }

    void UiRenderer::createRendererResources(const UiRendererConfig& config) {
        vertexShader_ = shaders_->load(ShaderId::ImGuiVertex);
        fragmentShader_ = shaders_->load(ShaderId::ImGuiFragment);

        const nvrhi::VertexAttributeDesc attributes[] = {
            nvrhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(ImDrawVert, pos))
                .setElementStride(sizeof(ImDrawVert)),
            nvrhi::VertexAttributeDesc()
                .setName("TEXCOORD")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(ImDrawVert, uv))
                .setElementStride(sizeof(ImDrawVert)),
            nvrhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(nvrhi::Format::RGBA8_UNORM)
                .setOffset(offsetof(ImDrawVert, col))
                .setElementStride(sizeof(ImDrawVert)),
        };
        inputLayout_ = device_->createInputLayout(attributes, std::size(attributes), vertexShader_);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.setVisibility(nvrhi::ShaderType::AllGraphics)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(UiPushConstants)))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0));
        bindingLayout_ = device_->createBindingLayout(layoutDesc);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setInputLayout(inputLayout_)
            .setVertexShader(vertexShader_)
            .setFragmentShader(fragmentShader_)
            .addBindingLayout(bindingLayout_);
        pipelineDesc.renderState.rasterState.setFillSolid().setCullNone().enableScissor();
        pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
        pipelineDesc.renderState.blendState.targets[0]
            .enableBlend()
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
            .setBlendOp(nvrhi::BlendOp::Add)
            .setSrcBlendAlpha(nvrhi::BlendFactor::One)
            .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
            .setBlendOpAlpha(nvrhi::BlendOp::Add);

        nvrhi::FramebufferInfo framebufferInfo;
        framebufferInfo.addColorFormat(config.colorFormat).setSampleCount(config.sampleCount);
        pipeline_ = device_->createGraphicsPipeline(pipelineDesc, framebufferInfo);
        if (!vertexShader_ || !fragmentShader_ || !inputLayout_ || !bindingLayout_ || !pipeline_) {
            throw std::runtime_error("Failed to create UiRenderer pipeline resources.");
        }

        frameBuffers_.resize(config.frameSlotCount);
        createFontResources(*config.fontAtlas);
    }

    void UiRenderer::createFontResources(ImFontAtlas& atlas) {
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (pixels == nullptr || width <= 0 || height <= 0) {
            throw std::invalid_argument("Dear ImGui produced an empty font atlas.");
        }
        nvrhi::TextureDesc textureDesc;
        textureDesc.setWidth(static_cast<std::uint32_t>(width))
            .setHeight(static_cast<std::uint32_t>(height))
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDebugName("UI font atlas")
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true);
        fontTexture_ = device_->createTexture(textureDesc);
        fontSampler_ =
            device_->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
        if (!fontTexture_ || !fontSampler_) {
            throw std::runtime_error("Failed to create UiRenderer font resources.");
        }

        nvrhi::CommandListHandle upload = device_->createCommandList();
        nvrhi::EventQueryHandle complete = device_->createEventQuery();
        if (!upload || !complete) {
            throw std::runtime_error("Failed to create UiRenderer font upload resources.");
        }
        upload->setEnableAutomaticBarriers(true);
        upload->open();
        const std::size_t rowPitch = static_cast<std::size_t>(width) * 4;
        upload->writeTexture(fontTexture_, 0, 0, pixels, rowPitch, rowPitch * static_cast<std::size_t>(height));
        upload->close();
        device_->executeCommandList(upload);
        device_->setEventQuery(complete, nvrhi::CommandQueue::Graphics);
        device_->waitEventQuery(complete);

        nvrhi::BindingSetHandle binding = createTextureBinding(fontTexture_);
        if (!binding) {
            throw std::runtime_error("Failed to create UiRenderer font binding set.");
        }
        textureBindings_.emplace(core::uiFontTextureId().value(), std::move(binding));
        // ImGui 1.92 的 legacy SetTexID 只能在 GetTexData 构建出 TexData 后调用。
        atlas.SetTexID(static_cast<ImTextureID>(core::uiFontTextureId().value()));
    }

    nvrhi::BindingSetHandle UiRenderer::createTextureBinding(nvrhi::ITexture* texture) const {
        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(UiPushConstants)))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture))
            .addItem(nvrhi::BindingSetItem::Sampler(0, fontSampler_));
        return device_->createBindingSet(desc, bindingLayout_);
    }

    void UiRenderer::registerTexture(core::UiTextureId id, nvrhi::ITexture* texture) {
        if (!initialized_ || !id.isValid() || id == core::uiFontTextureId() || texture == nullptr) {
            throw std::invalid_argument("UiRenderer requires a valid non-font texture registration.");
        }
        nvrhi::BindingSetHandle binding = createTextureBinding(texture);
        if (!binding) {
            throw std::runtime_error("Failed to create UiRenderer texture binding.");
        }
        textureBindings_.insert_or_assign(id.value(), std::move(binding));
    }

    void UiRenderer::unregisterTexture(core::UiTextureId id) noexcept {
        if (id.isValid() && id != core::uiFontTextureId()) {
            textureBindings_.erase(id.value());
        }
    }

    nvrhi::IBindingSet& UiRenderer::resolveTexture(core::UiTextureId id) const {
        const core::UiTextureId resolvedId = id.isValid() ? id : core::uiFontTextureId();
        const auto found = textureBindings_.find(resolvedId.value());
        if (found == textureBindings_.end() || !found->second) {
            throw std::logic_error("Dear ImGui references an unregistered logical texture id.");
        }
        return *found->second;
    }

    std::size_t UiRenderer::growBufferCapacity(std::size_t currentCapacity, std::size_t requiredCapacity,
                                               std::size_t minimumCapacity) noexcept {
        if (requiredCapacity <= currentCapacity) {
            return currentCapacity;
        }
        if (currentCapacity == 0) {
            return std::max(requiredCapacity, minimumCapacity);
        }
        const std::size_t growth = currentCapacity > std::numeric_limits<std::size_t>::max() - currentCapacity / 2
                                       ? std::numeric_limits<std::size_t>::max()
                                       : currentCapacity + currentCapacity / 2;
        return std::max(requiredCapacity, growth);
    }

    UiProjection UiRenderer::makeNvrhiProjection(float displayPosX, float displayPosY, float displayWidth,
                                                 float displayHeight) noexcept {
        const float scaleX = 2.0f / displayWidth;
        const float displayScaleY = 2.0f / displayHeight;
        return {scaleX, -displayScaleY, -1.0f - displayPosX * scaleX, 1.0f + displayPosY * displayScaleY};
    }

    void UiRenderer::ensureBuffers(FrameBuffers& buffers, std::size_t vertexCount, std::size_t indexCount) {
        const std::size_t vertexCapacity =
            growBufferCapacity(buffers.vertexCapacity, vertexCount, minimumVertexCapacity);
        if (vertexCapacity != buffers.vertexCapacity) {
            nvrhi::BufferDesc desc;
            desc.setByteSize(vertexCapacity * sizeof(ImDrawVert))
                .setDebugName("UI vertex buffer")
                .setIsVertexBuffer(true)
                .setCpuAccess(nvrhi::CpuAccessMode::Write)
                .setInitialState(nvrhi::ResourceStates::VertexBuffer);
            buffers.vertexBuffer = device_->createBuffer(desc);
            buffers.vertexCapacity = vertexCapacity;
        }

        const std::size_t indexCapacity = growBufferCapacity(buffers.indexCapacity, indexCount, minimumIndexCapacity);
        if (indexCapacity != buffers.indexCapacity) {
            nvrhi::BufferDesc desc;
            desc.setByteSize(indexCapacity * sizeof(ImDrawIdx))
                .setDebugName("UI index buffer")
                .setIsIndexBuffer(true)
                .setCpuAccess(nvrhi::CpuAccessMode::Write)
                .setInitialState(nvrhi::ResourceStates::IndexBuffer);
            buffers.indexBuffer = device_->createBuffer(desc);
            buffers.indexCapacity = indexCapacity;
        }
        if (!buffers.vertexBuffer || !buffers.indexBuffer) {
            throw std::runtime_error("Failed to create UiRenderer dynamic buffers.");
        }
    }

    void UiRenderer::setRenderState(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                    const ImDrawData& drawData, const FrameBuffers& buffers, const nvrhi::Rect& scissor,
                                    nvrhi::IBindingSet& textureBinding) {
        const float framebufferWidth = drawData.DisplaySize.x * drawData.FramebufferScale.x;
        const float framebufferHeight = drawData.DisplaySize.y * drawData.FramebufferScale.y;
        nvrhi::ViewportState viewport;
        viewport.addViewport(nvrhi::Viewport(framebufferWidth, framebufferHeight)).addScissorRect(scissor);

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline_)
            .setFramebuffer(&framebuffer)
            .setViewport(viewport)
            .addBindingSet(&textureBinding)
            .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(buffers.vertexBuffer).setSlot(0).setOffset(0))
            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(buffers.indexBuffer)
                                .setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT)
                                .setOffset(0));
        commandList.setGraphicsState(state);

        const UiProjection projection = makeNvrhiProjection(drawData.DisplayPos.x, drawData.DisplayPos.y,
                                                            drawData.DisplaySize.x, drawData.DisplaySize.y);
        const UiPushConstants constants = {
            {projection.scaleX, projection.scaleY},
            {projection.translateX, projection.translateY},
            {outputIsSrgb_ ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f},
        };
        commandList.setPushConstants(&constants, sizeof(constants));
    }

    void UiRenderer::render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, std::uint32_t frameSlot,
                            const ImDrawData& drawData) {
        if (!initialized_) {
            throw std::logic_error("UiRenderer is not initialized.");
        }
        const float framebufferWidthValue = drawData.DisplaySize.x * drawData.FramebufferScale.x;
        const float framebufferHeightValue = drawData.DisplaySize.y * drawData.FramebufferScale.y;
        if (!drawData.Valid || drawData.TotalVtxCount <= 0 || drawData.TotalIdxCount <= 0 ||
            framebufferWidthValue <= 0.0f || framebufferHeightValue <= 0.0f) {
            return;
        }

        FrameBuffers& buffers = frameBuffers_[frameSlot % frameBuffers_.size()];
        ensureBuffers(buffers, static_cast<std::size_t>(drawData.TotalVtxCount),
                      static_cast<std::size_t>(drawData.TotalIdxCount));
        void* mappedVertices = device_->mapBuffer(buffers.vertexBuffer, nvrhi::CpuAccessMode::Write);
        void* mappedIndices = device_->mapBuffer(buffers.indexBuffer, nvrhi::CpuAccessMode::Write);
        if (mappedVertices == nullptr || mappedIndices == nullptr) {
            if (mappedVertices != nullptr) {
                device_->unmapBuffer(buffers.vertexBuffer);
            }
            if (mappedIndices != nullptr) {
                device_->unmapBuffer(buffers.indexBuffer);
            }
            throw std::runtime_error("Failed to map a UiRenderer dynamic buffer.");
        }
        auto* vertexDestination = static_cast<std::byte*>(mappedVertices);
        auto* indexDestination = static_cast<std::byte*>(mappedIndices);
        for (const ImDrawList* list : drawData.CmdLists) {
            const std::size_t vertexBytes = list->VtxBuffer.size() * sizeof(ImDrawVert);
            const std::size_t indexBytes = list->IdxBuffer.size() * sizeof(ImDrawIdx);
            std::memcpy(vertexDestination, list->VtxBuffer.Data, vertexBytes);
            std::memcpy(indexDestination, list->IdxBuffer.Data, indexBytes);
            vertexDestination += vertexBytes;
            indexDestination += indexBytes;
        }
        device_->unmapBuffer(buffers.vertexBuffer);
        device_->unmapBuffer(buffers.indexBuffer);

        const int framebufferWidth = static_cast<int>(framebufferWidthValue);
        const int framebufferHeight = static_cast<int>(framebufferHeightValue);
        const nvrhi::Rect fullScissor(framebufferWidth, framebufferHeight);
        nvrhi::IBindingSet& fontBinding = resolveTexture(core::uiFontTextureId());
        setRenderState(commandList, framebuffer, drawData, buffers, fullScissor, fontBinding);

        std::uint32_t globalIndexOffset = 0;
        std::uint32_t globalVertexOffset = 0;
        for (const ImDrawList* list : drawData.CmdLists) {
            for (const ImDrawCmd& draw : list->CmdBuffer) {
                if (draw.UserCallback == ImDrawCallback_ResetRenderState) {
                    setRenderState(commandList, framebuffer, drawData, buffers, fullScissor, fontBinding);
                    continue;
                }
                if (draw.UserCallback != nullptr) {
                    draw.UserCallback(list, &draw);
                    continue;
                }
                const float left = std::clamp((draw.ClipRect.x - drawData.DisplayPos.x) * drawData.FramebufferScale.x,
                                              0.0f, framebufferWidthValue);
                const float top = std::clamp((draw.ClipRect.y - drawData.DisplayPos.y) * drawData.FramebufferScale.y,
                                             0.0f, framebufferHeightValue);
                const float right = std::clamp((draw.ClipRect.z - drawData.DisplayPos.x) * drawData.FramebufferScale.x,
                                               0.0f, framebufferWidthValue);
                const float bottom = std::clamp((draw.ClipRect.w - drawData.DisplayPos.y) * drawData.FramebufferScale.y,
                                                0.0f, framebufferHeightValue);
                if (draw.ElemCount == 0 || !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
                    !std::isfinite(bottom) || right <= left || bottom <= top) {
                    continue;
                }
                const nvrhi::Rect scissor(static_cast<int>(std::floor(left)), static_cast<int>(std::ceil(right)),
                                          static_cast<int>(std::floor(top)), static_cast<int>(std::ceil(bottom)));
                const core::UiTextureId texture{static_cast<core::UiTextureId::ValueType>(draw.GetTexID())};
                nvrhi::IBindingSet& textureBinding = resolveTexture(texture);
                setRenderState(commandList, framebuffer, drawData, buffers, scissor, textureBinding);
                nvrhi::DrawArguments arguments;
                arguments.setVertexCount(draw.ElemCount)
                    .setStartIndexLocation(globalIndexOffset + draw.IdxOffset)
                    .setStartVertexLocation(globalVertexOffset + draw.VtxOffset);
                commandList.drawIndexed(arguments);
            }
            globalIndexOffset += static_cast<std::uint32_t>(list->IdxBuffer.size());
            globalVertexOffset += static_cast<std::uint32_t>(list->VtxBuffer.size());
        }
    }

    nvrhi::ITexture* UiRenderer::fontTexture() const noexcept {
        return fontTexture_;
    }

    nvrhi::ResourceStates UiRenderer::fontTextureInitialState() const noexcept {
        return nvrhi::ResourceStates::ShaderResource;
    }

    bool UiRenderer::initialized() const noexcept {
        return initialized_;
    }

} // namespace lumin::render

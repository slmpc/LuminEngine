#include "render/presentation/UiRenderer.hpp"

#include "render/resources/ShaderLibrary.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

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
        if (config.device == nullptr || config.colorFormat == nvrhi::Format::UNKNOWN || config.frameSlotCount == 0 ||
            config.fontAtlas == nullptr || !config.fontAtlas->isValid()) {
            throw std::invalid_argument("Invalid UiRenderer configuration.");
        }
        device_ = config.device;
        shaderDirectory_ = config.shaderDirectory;
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
        shaderDirectory_.clear();
        device_ = nullptr;
        outputIsSrgb_ = false;
        initialized_ = false;
    }

    void UiRenderer::createRendererResources(const UiRendererConfig& config) {
        ShaderLibrary shaders(*device_, shaderDirectory_);
        vertexShader_ = shaders.loadModule("ImGui.vert.spv", nvrhi::ShaderType::Vertex, "vertexMain");
        fragmentShader_ = shaders.loadModule("ImGui.frag.spv", nvrhi::ShaderType::Pixel, "fragmentMain");

        const nvrhi::VertexAttributeDesc attributes[] = {
            nvrhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(core::UiVertex, positionX))
                .setElementStride(sizeof(core::UiVertex)),
            nvrhi::VertexAttributeDesc()
                .setName("TEXCOORD")
                .setFormat(nvrhi::Format::RG32_FLOAT)
                .setOffset(offsetof(core::UiVertex, textureU))
                .setElementStride(sizeof(core::UiVertex)),
            nvrhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(nvrhi::Format::RGBA8_UNORM)
                .setOffset(offsetof(core::UiVertex, color))
                .setElementStride(sizeof(core::UiVertex)),
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

    void UiRenderer::createFontResources(const core::UiFontAtlas& atlas) {
        nvrhi::TextureDesc textureDesc;
        textureDesc.setWidth(atlas.width)
            .setHeight(atlas.height)
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
        upload->writeTexture(fontTexture_, 0, 0, atlas.rgba8.data(), static_cast<std::size_t>(atlas.width) * 4,
                             atlas.rgba8.size());
        upload->close();
        device_->executeCommandList(upload);
        device_->setEventQuery(complete, nvrhi::CommandQueue::Graphics);
        device_->waitEventQuery(complete);

        nvrhi::BindingSetHandle binding = createTextureBinding(fontTexture_);
        if (!binding) {
            throw std::runtime_error("Failed to create UiRenderer font binding set.");
        }
        textureBindings_.emplace(core::uiFontTextureId().value(), std::move(binding));
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
            throw std::logic_error("UiDrawPacket references an unregistered logical texture id.");
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
            desc.setByteSize(vertexCapacity * sizeof(core::UiVertex))
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
            desc.setByteSize(indexCapacity * sizeof(std::uint32_t))
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
                                    const core::UiDrawPacket& packet, const FrameBuffers& buffers,
                                    const nvrhi::Rect& scissor, nvrhi::IBindingSet& textureBinding) {
        const float framebufferWidth = packet.displayWidth * packet.framebufferScaleX;
        const float framebufferHeight = packet.displayHeight * packet.framebufferScaleY;
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
                                .setFormat(nvrhi::Format::R32_UINT)
                                .setOffset(0));
        commandList.setGraphicsState(state);

        const UiProjection projection = makeNvrhiProjection(packet.displayPositionX, packet.displayPositionY,
                                                            packet.displayWidth, packet.displayHeight);
        const UiPushConstants constants = {
            {projection.scaleX, projection.scaleY},
            {projection.translateX, projection.translateY},
            {outputIsSrgb_ ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f},
        };
        commandList.setPushConstants(&constants, sizeof(constants));
    }

    void UiRenderer::render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, std::uint32_t frameSlot,
                            const core::UiDrawPacket& packet) {
        if (!initialized_) {
            throw std::logic_error("UiRenderer is not initialized.");
        }
        if (!packet.isRenderable()) {
            return;
        }

        FrameBuffers& buffers = frameBuffers_[frameSlot % frameBuffers_.size()];
        ensureBuffers(buffers, packet.vertices.size(), packet.indices.size());
        const auto writeMapped = [this](nvrhi::IBuffer* buffer, const void* data, std::size_t size) {
            void* mapped = device_->mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map a UiRenderer dynamic buffer.");
            }
            std::memcpy(mapped, data, size);
            device_->unmapBuffer(buffer);
        };
        writeMapped(buffers.vertexBuffer, packet.vertices.data(), packet.vertices.size() * sizeof(core::UiVertex));
        writeMapped(buffers.indexBuffer, packet.indices.data(), packet.indices.size() * sizeof(std::uint32_t));

        const int framebufferWidth = static_cast<int>(packet.displayWidth * packet.framebufferScaleX);
        const int framebufferHeight = static_cast<int>(packet.displayHeight * packet.framebufferScaleY);
        const nvrhi::Rect fullScissor(framebufferWidth, framebufferHeight);
        nvrhi::IBindingSet& fontBinding = resolveTexture(core::uiFontTextureId());
        setRenderState(commandList, framebuffer, packet, buffers, fullScissor, fontBinding);

        for (const core::UiDrawCommand& draw : packet.commands) {
            if (draw.type == core::UiDrawCommandType::ResetRenderState) {
                setRenderState(commandList, framebuffer, packet, buffers, fullScissor, fontBinding);
                continue;
            }
            const nvrhi::Rect scissor(draw.scissorLeft, draw.scissorRight, draw.scissorTop, draw.scissorBottom);
            nvrhi::IBindingSet& textureBinding = resolveTexture(draw.texture);
            setRenderState(commandList, framebuffer, packet, buffers, scissor, textureBinding);
            nvrhi::DrawArguments arguments;
            arguments.setVertexCount(draw.elementCount)
                .setStartIndexLocation(draw.indexOffset)
                .setStartVertexLocation(draw.vertexOffset);
            commandList.drawIndexed(arguments);
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

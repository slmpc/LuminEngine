#include "render/ImGuiLayer.hpp"

#include "render/platform/Window.hpp"
#include "render/ShaderLibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

namespace lumin::render {
    namespace {

        constexpr std::size_t minimumVertexCapacity = 5'000;
        constexpr std::size_t minimumIndexCapacity = 10'000;
        struct ImGuiPushConstants {
            float scale[2];
            float translate[2];
        };

        [[nodiscard]] bool finiteRect(const ImVec4& rect) {
            return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.z) && std::isfinite(rect.w);
        }

    } // namespace

    ImGuiLayer::~ImGuiLayer() {
        shutdown();
    }

    void ImGuiLayer::initialize(platform::Window& window, const ImGuiLayerConfig& config) {
        shutdown();
        if (config.device == nullptr || config.colorFormat == nvrhi::Format::UNKNOWN || config.frameSlotCount == 0) {
            throw std::invalid_argument("Invalid NvRHI ImGui layer configuration.");
        }

        device_ = config.device;
        shaderDirectory_ = config.shaderDirectory;
        window_ = &window;

        try {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            contextCreated_ = true;

            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            if (config.enableKeyboard) {
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            }
            if (config.enableGamepad) {
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            }
            if (config.enableDocking) {
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            }
            io.FontGlobalScale = config.globalScale;
            io.BackendRendererName = "lumin_nvrhi";
            io.BackendRendererUserData = this;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            ImGui::StyleColorsDark();

            createRendererResources(config);
            if (!ImGui_ImplSDL3_InitForVulkan(window.nativeHandle())) {
                throw std::runtime_error("Failed to initialize Dear ImGui SDL backend.");
            }
            sdlInitialized_ = true;
            window.setEventCallback([](const SDL_Event& event) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            });
            initialized_ = true;
        } catch (...) {
            shutdown();
            throw;
        }
    }

    void ImGuiLayer::createRendererResources(const ImGuiLayerConfig& config) {
        ShaderLibrary shaders(*device_, shaderDirectory_);
        vertexShader_ = shaders.loadModule("imgui.vert.spv", nvrhi::ShaderType::Vertex, "vertexMain");
        fragmentShader_ = shaders.loadModule("imgui.frag.spv", nvrhi::ShaderType::Pixel, "fragmentMain");

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
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ImGuiPushConstants)))
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
            throw std::runtime_error("Failed to create NvRHI ImGui pipeline resources.");
        }

        frameBuffers_.resize(config.frameSlotCount);
        createFontResources();
    }

    void ImGuiLayer::createFontResources() {
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        if (pixels == nullptr || width <= 0 || height <= 0) {
            throw std::runtime_error("Dear ImGui produced an empty font atlas.");
        }

        nvrhi::TextureDesc textureDesc;
        textureDesc.setWidth(static_cast<std::uint32_t>(width))
            .setHeight(static_cast<std::uint32_t>(height))
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDebugName("ImGui font atlas")
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true);
        fontTexture_ = device_->createTexture(textureDesc);
        fontSampler_ =
            device_->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
        if (!fontTexture_ || !fontSampler_) {
            throw std::runtime_error("Failed to create NvRHI ImGui font resources.");
        }

        nvrhi::CommandListHandle upload = device_->createCommandList();
        nvrhi::EventQueryHandle complete = device_->createEventQuery();
        if (!upload || !complete) {
            throw std::runtime_error("Failed to create NvRHI ImGui font upload resources.");
        }
        upload->setEnableAutomaticBarriers(true);
        upload->open();
        upload->writeTexture(fontTexture_, 0, 0, pixels, static_cast<std::size_t>(width) * 4,
                             static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        upload->close();
        device_->executeCommandList(upload);
        device_->setEventQuery(complete, nvrhi::CommandQueue::Graphics);
        device_->waitEventQuery(complete);

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ImGuiPushConstants)))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, fontTexture_))
            .addItem(nvrhi::BindingSetItem::Sampler(0, fontSampler_));
        bindingSet_ = device_->createBindingSet(bindingSetDesc, bindingLayout_);
        if (!bindingSet_) {
            throw std::runtime_error("Failed to create NvRHI ImGui font binding set.");
        }
        ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(bindingSet_.Get())));
    }

    void ImGuiLayer::shutdown() {
        if (window_ != nullptr) {
            window_->setEventCallback({});
        }
        if (sdlInitialized_) {
            ImGui_ImplSDL3_Shutdown();
            sdlInitialized_ = false;
        }
        if (contextCreated_) {
            ImGuiIO& io = ImGui::GetIO();
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
            io.BackendRendererName = nullptr;
            io.BackendRendererUserData = nullptr;
            ImGui::DestroyContext();
            contextCreated_ = false;
        }

        frameBuffers_.clear();
        pipeline_ = nullptr;
        inputLayout_ = nullptr;
        vertexShader_ = nullptr;
        fragmentShader_ = nullptr;
        bindingSet_ = nullptr;
        bindingLayout_ = nullptr;
        fontSampler_ = nullptr;
        fontTexture_ = nullptr;
        fontTextureInitialized_ = false;
        shaderDirectory_.clear();
        device_ = nullptr;
        window_ = nullptr;
        initialized_ = false;
    }

    void ImGuiLayer::newFrame() {
        if (!initialized_) {
            return;
        }
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    std::size_t ImGuiLayer::growBufferCapacity(std::size_t currentCapacity, std::size_t requiredCapacity,
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

    ImGuiProjection ImGuiLayer::makeNvrhiProjection(float displayPosX, float displayPosY, float displayWidth,
                                                    float displayHeight) noexcept {
        const float scaleX = 2.0f / displayWidth;
        const float displayScaleY = 2.0f / displayHeight;
        return {scaleX, -displayScaleY, -1.0f - displayPosX * scaleX, 1.0f + displayPosY * displayScaleY};
    }

    std::vector<ImGuiDrawEvent> ImGuiLayer::buildDrawEvents(const ImDrawData& drawData) {
        std::vector<ImGuiDrawEvent> events;
        const float framebufferWidth = drawData.DisplaySize.x * drawData.FramebufferScale.x;
        const float framebufferHeight = drawData.DisplaySize.y * drawData.FramebufferScale.y;
        if (!drawData.Valid || framebufferWidth <= 0.0f || framebufferHeight <= 0.0f) {
            return events;
        }

        std::uint32_t globalIndexOffset = 0;
        std::uint32_t globalVertexOffset = 0;
        for (const ImDrawList* list : drawData.CmdLists) {
            for (const ImDrawCmd& command : list->CmdBuffer) {
                ImGuiDrawEvent event;
                event.list = list;
                event.command = &command;
                if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                    event.type = ImGuiDrawEvent::Type::ResetRenderState;
                    events.push_back(event);
                    continue;
                }
                if (command.UserCallback != nullptr) {
                    event.type = ImGuiDrawEvent::Type::UserCallback;
                    events.push_back(event);
                    continue;
                }
                if (command.ElemCount == 0 || !finiteRect(command.ClipRect)) {
                    continue;
                }

                const float left = std::clamp(
                    (command.ClipRect.x - drawData.DisplayPos.x) * drawData.FramebufferScale.x, 0.0f, framebufferWidth);
                const float top = std::clamp((command.ClipRect.y - drawData.DisplayPos.y) * drawData.FramebufferScale.y,
                                             0.0f, framebufferHeight);
                const float right = std::clamp(
                    (command.ClipRect.z - drawData.DisplayPos.x) * drawData.FramebufferScale.x, 0.0f, framebufferWidth);
                const float bottom =
                    std::clamp((command.ClipRect.w - drawData.DisplayPos.y) * drawData.FramebufferScale.y, 0.0f,
                               framebufferHeight);
                if (right <= left || bottom <= top) {
                    continue;
                }

                event.type = ImGuiDrawEvent::Type::Draw;
                event.scissorLeft = static_cast<std::int32_t>(std::floor(left));
                event.scissorTop = static_cast<std::int32_t>(std::floor(top));
                event.scissorRight = static_cast<std::int32_t>(std::ceil(right));
                event.scissorBottom = static_cast<std::int32_t>(std::ceil(bottom));
                event.elementCount = command.ElemCount;
                event.indexOffset = globalIndexOffset + command.IdxOffset;
                event.vertexOffset = globalVertexOffset + command.VtxOffset;
                events.push_back(event);
            }
            globalIndexOffset += static_cast<std::uint32_t>(list->IdxBuffer.Size);
            globalVertexOffset += static_cast<std::uint32_t>(list->VtxBuffer.Size);
        }
        return events;
    }

    void ImGuiLayer::ensureBuffers(FrameBuffers& buffers, std::size_t vertexCount, std::size_t indexCount) {
        const std::size_t vertexCapacity =
            growBufferCapacity(buffers.vertexCapacity, vertexCount, minimumVertexCapacity);
        if (vertexCapacity != buffers.vertexCapacity) {
            nvrhi::BufferDesc desc;
            desc.setByteSize(vertexCapacity * sizeof(ImDrawVert))
                .setDebugName("ImGui vertex buffer")
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
                .setDebugName("ImGui index buffer")
                .setIsIndexBuffer(true)
                .setCpuAccess(nvrhi::CpuAccessMode::Write)
                .setInitialState(nvrhi::ResourceStates::IndexBuffer);
            buffers.indexBuffer = device_->createBuffer(desc);
            buffers.indexCapacity = indexCapacity;
        }
        if (!buffers.vertexBuffer || !buffers.indexBuffer) {
            throw std::runtime_error("Failed to create NvRHI ImGui dynamic buffers.");
        }
    }

    void ImGuiLayer::setRenderState(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                    const ImDrawData& drawData, const FrameBuffers& buffers,
                                    const nvrhi::Rect& scissor) {
        const float framebufferWidth = drawData.DisplaySize.x * drawData.FramebufferScale.x;
        const float framebufferHeight = drawData.DisplaySize.y * drawData.FramebufferScale.y;
        nvrhi::ViewportState viewport;
        viewport.addViewport(nvrhi::Viewport(framebufferWidth, framebufferHeight)).addScissorRect(scissor);

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline_)
            .setFramebuffer(&framebuffer)
            .setViewport(viewport)
            .addBindingSet(bindingSet_)
            .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(buffers.vertexBuffer).setSlot(0).setOffset(0))
            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(buffers.indexBuffer)
                                .setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT)
                                .setOffset(0));
        commandList.setGraphicsState(state);

        const ImGuiProjection projection = makeNvrhiProjection(drawData.DisplayPos.x, drawData.DisplayPos.y,
                                                               drawData.DisplaySize.x, drawData.DisplaySize.y);
        const ImGuiPushConstants constants = {
            {projection.scaleX, projection.scaleY},
            {projection.translateX, projection.translateY},
        };
        commandList.setPushConstants(&constants, sizeof(constants));
    }

    void ImGuiLayer::render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                            std::uint32_t frameSlot) {
        if (!initialized_ || frameBuffers_.empty()) {
            return;
        }

        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData == nullptr || drawData->TotalVtxCount <= 0 || drawData->TotalIdxCount <= 0) {
            return;
        }

        FrameBuffers& buffers = frameBuffers_[frameSlot % frameBuffers_.size()];
        ensureBuffers(buffers, static_cast<std::size_t>(drawData->TotalVtxCount),
                      static_cast<std::size_t>(drawData->TotalIdxCount));

        std::vector<ImDrawVert> vertices;
        std::vector<ImDrawIdx> indices;
        vertices.reserve(static_cast<std::size_t>(drawData->TotalVtxCount));
        indices.reserve(static_cast<std::size_t>(drawData->TotalIdxCount));
        for (const ImDrawList* list : drawData->CmdLists) {
            vertices.insert(vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
            indices.insert(indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
        }
        const auto writeMapped = [this](nvrhi::IBuffer* buffer, const void* data, std::size_t size) {
            void* mapped = device_->mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map an NvRHI ImGui dynamic buffer.");
            }
            std::memcpy(mapped, data, size);
            device_->unmapBuffer(buffer);
        };
        writeMapped(buffers.vertexBuffer, vertices.data(), vertices.size() * sizeof(ImDrawVert));
        writeMapped(buffers.indexBuffer, indices.data(), indices.size() * sizeof(ImDrawIdx));

        const int framebufferWidth = static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
        const int framebufferHeight = static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
        const nvrhi::Rect fullScissor(framebufferWidth, framebufferHeight);
        setRenderState(commandList, framebuffer, *drawData, buffers, fullScissor);

        for (const ImGuiDrawEvent& event : buildDrawEvents(*drawData)) {
            if (event.type == ImGuiDrawEvent::Type::ResetRenderState) {
                setRenderState(commandList, framebuffer, *drawData, buffers, fullScissor);
            } else if (event.type == ImGuiDrawEvent::Type::UserCallback) {
                event.command->UserCallback(event.list, event.command);
            } else {
                const nvrhi::Rect scissor(event.scissorLeft, event.scissorRight, event.scissorTop, event.scissorBottom);
                setRenderState(commandList, framebuffer, *drawData, buffers, scissor);
                nvrhi::DrawArguments arguments;
                arguments.setVertexCount(event.elementCount)
                    .setStartIndexLocation(event.indexOffset)
                    .setStartVertexLocation(event.vertexOffset);
                commandList.drawIndexed(arguments);
            }
        }
    }

    bool ImGuiLayer::initialized() const noexcept {
        return initialized_;
    }

    ImGuiCaptureState ImGuiLayer::captureState() const noexcept {
        if (!initialized_) {
            return {};
        }
        const ImGuiIO& io = ImGui::GetIO();
        return {io.WantCaptureKeyboard, io.WantCaptureMouse, io.WantTextInput};
    }

    nvrhi::ITexture* ImGuiLayer::fontTexture() const noexcept {
        return fontTexture_;
    }

    nvrhi::ResourceStates ImGuiLayer::fontTextureInitialState() const noexcept {
        return nvrhi::ResourceStates::ShaderResource;
    }

    void ImGuiLayer::markFontTextureInitialized() noexcept {
        fontTextureInitialized_ = true;
    }

} // namespace lumin::render

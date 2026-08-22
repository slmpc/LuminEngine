#include "render/editor/ImGuiFrontend.hpp"

#include "render/platform/Window.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

namespace lumin::render {
    namespace {

        [[nodiscard]] bool finiteRect(const ImVec4& rect) noexcept {
            return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.z) && std::isfinite(rect.w);
        }

        [[nodiscard]] std::uint32_t checkedU32(std::size_t value, const char* diagnostic) {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(diagnostic);
            }
            return static_cast<std::uint32_t>(value);
        }

    } // namespace

    ImGuiFrontend::~ImGuiFrontend() {
        shutdown();
    }

    void ImGuiFrontend::initialize(platform::Window& window) {
        shutdown();
        window_ = &window;
        try {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            contextCreated_ = true;

            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
            io.BackendRendererName = "lumin_ui_packet";
            io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            io.FontGlobalScale = 1.0f;
            ImGui::StyleColorsDark();

            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            if (pixels == nullptr || width <= 0 || height <= 0) {
                throw std::runtime_error("Dear ImGui produced an empty font atlas.");
            }
            fontAtlas_.width = static_cast<std::uint32_t>(width);
            fontAtlas_.height = static_cast<std::uint32_t>(height);
            const std::size_t byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
            fontAtlas_.rgba8.assign(pixels, pixels + byteCount);
            io.Fonts->SetTexID(static_cast<ImTextureID>(core::uiFontTextureId().value()));

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

    void ImGuiFrontend::shutdown() noexcept {
        cancelFrame();
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
            ImGui::DestroyContext();
            contextCreated_ = false;
        }
        fontAtlas_ = {};
        window_ = nullptr;
        initialized_ = false;
    }

    void ImGuiFrontend::beginFrame(ImGuiContent* content) {
        if (!initialized_) {
            throw std::logic_error("ImGui frontend is not initialized.");
        }
        if (frameActive_) {
            throw std::logic_error("ImGui frontend already has an active frame.");
        }
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frameActive_ = true;
        try {
            if (content != nullptr) {
                content->draw();
            }
        } catch (...) {
            cancelFrame();
            throw;
        }
    }

    core::UiDrawPacket ImGuiFrontend::finishFrame() {
        if (!frameActive_) {
            throw std::logic_error("ImGui frontend has no active frame to finish.");
        }
        ImGui::Render();
        frameActive_ = false;
        const ImDrawData* drawData = ImGui::GetDrawData();
        return drawData == nullptr ? core::UiDrawPacket{} : buildDrawPacket(*drawData);
    }

    void ImGuiFrontend::cancelFrame() noexcept {
        if (frameActive_) {
            ImGui::EndFrame();
            frameActive_ = false;
        }
    }

    ImGuiCaptureState ImGuiFrontend::captureState() const noexcept {
        if (!initialized_) {
            return {};
        }
        const ImGuiIO& io = ImGui::GetIO();
        return {io.WantCaptureKeyboard, io.WantCaptureMouse, io.WantTextInput};
    }

    const core::UiFontAtlas& ImGuiFrontend::fontAtlas() const noexcept {
        return fontAtlas_;
    }

    bool ImGuiFrontend::initialized() const noexcept {
        return initialized_;
    }

    bool ImGuiFrontend::frameActive() const noexcept {
        return frameActive_;
    }

    core::UiDrawPacket ImGuiFrontend::buildDrawPacket(const ImDrawData& drawData) {
        core::UiDrawPacket packet;
        packet.displayPositionX = drawData.DisplayPos.x;
        packet.displayPositionY = drawData.DisplayPos.y;
        packet.displayWidth = drawData.DisplaySize.x;
        packet.displayHeight = drawData.DisplaySize.y;
        packet.framebufferScaleX = drawData.FramebufferScale.x;
        packet.framebufferScaleY = drawData.FramebufferScale.y;

        const float framebufferWidth = packet.displayWidth * packet.framebufferScaleX;
        const float framebufferHeight = packet.displayHeight * packet.framebufferScaleY;
        if (!drawData.Valid || framebufferWidth <= 0.0f || framebufferHeight <= 0.0f) {
            return packet;
        }

        packet.vertices.reserve(static_cast<std::size_t>(std::max(drawData.TotalVtxCount, 0)));
        packet.indices.reserve(static_cast<std::size_t>(std::max(drawData.TotalIdxCount, 0)));
        for (const ImDrawList* list : drawData.CmdLists) {
            for (const ImDrawVert& vertex : list->VtxBuffer) {
                packet.vertices.push_back({vertex.pos.x, vertex.pos.y, vertex.uv.x, vertex.uv.y, vertex.col});
            }
            for (const ImDrawIdx index : list->IdxBuffer) {
                packet.indices.push_back(static_cast<std::uint32_t>(index));
            }
        }

        std::uint32_t globalIndexOffset = 0;
        std::uint32_t globalVertexOffset = 0;
        for (const ImDrawList* list : drawData.CmdLists) {
            for (const ImDrawCmd& command : list->CmdBuffer) {
                if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                    packet.commands.push_back(core::UiDrawCommand{
                        .type = core::UiDrawCommandType::ResetRenderState,
                        .texture = {},
                    });
                    continue;
                }
                if (command.UserCallback != nullptr) {
                    throw std::invalid_argument("UiDrawPacket forbids arbitrary Dear ImGui user callbacks.");
                }
                if (command.ElemCount == 0 || !finiteRect(command.ClipRect)) {
                    continue;
                }

                const float left = std::clamp((command.ClipRect.x - packet.displayPositionX) * packet.framebufferScaleX,
                                              0.0f, framebufferWidth);
                const float top = std::clamp((command.ClipRect.y - packet.displayPositionY) * packet.framebufferScaleY,
                                             0.0f, framebufferHeight);
                const float right = std::clamp(
                    (command.ClipRect.z - packet.displayPositionX) * packet.framebufferScaleX, 0.0f, framebufferWidth);
                const float bottom = std::clamp(
                    (command.ClipRect.w - packet.displayPositionY) * packet.framebufferScaleY, 0.0f, framebufferHeight);
                if (right <= left || bottom <= top) {
                    continue;
                }

                packet.commands.push_back(core::UiDrawCommand{
                    .type = core::UiDrawCommandType::Draw,
                    .scissorLeft = static_cast<std::int32_t>(std::floor(left)),
                    .scissorTop = static_cast<std::int32_t>(std::floor(top)),
                    .scissorRight = static_cast<std::int32_t>(std::ceil(right)),
                    .scissorBottom = static_cast<std::int32_t>(std::ceil(bottom)),
                    .elementCount = command.ElemCount,
                    .indexOffset = globalIndexOffset + command.IdxOffset,
                    .vertexOffset = globalVertexOffset + command.VtxOffset,
                    .texture = core::UiTextureId{static_cast<core::UiTextureId::ValueType>(command.GetTexID())},
                });
            }
            globalIndexOffset += checkedU32(list->IdxBuffer.size(), "UiDrawPacket index offset overflow.");
            globalVertexOffset += checkedU32(list->VtxBuffer.size(), "UiDrawPacket vertex offset overflow.");
        }
        return packet;
    }

} // namespace lumin::render

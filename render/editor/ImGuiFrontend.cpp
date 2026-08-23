#include "render/editor/ImGuiFrontend.hpp"

#include "render/platform/Window.hpp"

#include <stdexcept>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

namespace lumin::render {
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
            io.BackendRendererName = "lumin_nvrhi_imgui";
            io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            io.FontGlobalScale = 1.0f;
            ImGui::StyleColorsDark();

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

    const ImDrawData* ImGuiFrontend::finishFrame() {
        if (!frameActive_) {
            throw std::logic_error("ImGui frontend has no active frame to finish.");
        }
        ImGui::Render();
        frameActive_ = false;
        return ImGui::GetDrawData();
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

    ImFontAtlas& ImGuiFrontend::fontAtlas() const {
        if (!initialized_) {
            throw std::logic_error("ImGui frontend is not initialized.");
        }
        return *ImGui::GetIO().Fonts;
    }

    bool ImGuiFrontend::initialized() const noexcept {
        return initialized_;
    }

    bool ImGuiFrontend::frameActive() const noexcept {
        return frameActive_;
    }

} // namespace lumin::render

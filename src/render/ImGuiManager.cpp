#include "render/ImGuiManager.hpp"

#include "platform/Window.hpp"
#include "render/VulkanContext.hpp"

#include <stdexcept>
#include <utility>

#include <imgui.h>

namespace lumin::render {

    ImGuiManager::~ImGuiManager() {
        shutdown();
    }

    void ImGuiManager::initialize(platform::Window& window, VulkanContext& context) {
        shutdown();

        ImGuiLayerConfig config;
        config.device = context.rhiDevice();
        config.colorFormat = context.swapchainRhiFormat();
        config.shaderDirectory = LUMIN_SHADER_DIR;
        config.enableKeyboard = true;
        config.enableDocking = true;
        layer_.initialize(window, config);

        framebuffers_.reserve(context.swapchainTextures().size());
        for (const nvrhi::TextureHandle& texture : context.swapchainTextures()) {
            nvrhi::FramebufferHandle framebuffer =
                context.rhiDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
            if (!framebuffer) {
                shutdown();
                throw std::runtime_error("Failed to create NvRHI ImGui framebuffer.");
            }
            framebuffers_.push_back(std::move(framebuffer));
        }
    }

    void ImGuiManager::shutdown() {
        cancelFrame();
        framebuffers_.clear();
        layer_.shutdown();
    }

    void ImGuiManager::beginFrame(ImGuiContent* content) {
        if (!layer_.initialized() || framePrepared_) {
            return;
        }
        layer_.newFrame();
        framePrepared_ = true;
        try {
            if (content != nullptr) {
                content->draw();
            }
        } catch (...) {
            cancelFrame();
            throw;
        }
    }

    void ImGuiManager::cancelFrame() noexcept {
        if (!framePrepared_) {
            return;
        }
        ImGui::EndFrame();
        framePrepared_ = false;
    }

    void ImGuiManager::record(nvrhi::ICommandList& commandList, std::uint32_t imageIndex, std::uint32_t frameSlot) {
        if (!layer_.initialized() || !framePrepared_ || imageIndex >= framebuffers_.size()) {
            return;
        }
        layer_.render(commandList, *framebuffers_[imageIndex], frameSlot);
        framePrepared_ = false;
    }

    bool ImGuiManager::framePrepared() const noexcept {
        return framePrepared_;
    }

    ImGuiCaptureState ImGuiManager::captureState() const noexcept {
        return layer_.captureState();
    }

    nvrhi::ITexture* ImGuiManager::fontTexture() const noexcept {
        return layer_.fontTexture();
    }

    nvrhi::ResourceStates ImGuiManager::fontTextureInitialState() const noexcept {
        return layer_.fontTextureInitialState();
    }

    void ImGuiManager::markFontTextureInitialized() noexcept {
        layer_.markFontTextureInitialized();
    }

}

#pragma once

#include <cstdint>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "render/editor/ImGuiContent.hpp"
#include "render/editor/ImGuiLayer.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::render {
    class VulkanContext;

    class ImGuiManager {
    public:
        ImGuiManager() = default;
        ~ImGuiManager();

        ImGuiManager(const ImGuiManager&) = delete;
        ImGuiManager& operator=(const ImGuiManager&) = delete;

        void initialize(platform::Window& window, VulkanContext& context);
        void shutdown();
        void beginFrame(ImGuiContent* content = nullptr);
        void cancelFrame() noexcept;
        void record(nvrhi::ICommandList& commandList, std::uint32_t imageIndex, std::uint32_t frameSlot);
        [[nodiscard]] bool framePrepared() const noexcept;
        [[nodiscard]] ImGuiCaptureState captureState() const noexcept;
        [[nodiscard]] nvrhi::ITexture* fontTexture() const noexcept;
        [[nodiscard]] nvrhi::ResourceStates fontTextureInitialState() const noexcept;
        void markFontTextureInitialized() noexcept;
        /** 更新 Viewport 图像 binding；传入空纹理会移除图像。 */
        void setViewportTexture(nvrhi::ITexture* texture);
        [[nodiscard]] std::uintptr_t viewportTextureId() const noexcept;

    private:
        ImGuiLayer layer_;
        std::vector<nvrhi::FramebufferHandle> framebuffers_;
        nvrhi::BindingSetHandle viewportBindingSet_;
        bool framePrepared_ = false;
    };

} // namespace lumin::render

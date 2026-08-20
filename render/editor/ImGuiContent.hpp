#pragma once

#include <cstdint>

namespace lumin::render {

    /** 可由 ImGui 窗口展示的渲染输出。尺寸使用物理像素。 */
    struct ImGuiViewportImage {
        std::uintptr_t textureId = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        [[nodiscard]] bool isValid() const noexcept {
            return textureId != 0 && width != 0 && height != 0;
        }
    };

    struct ImGuiCaptureState {
        bool wantCaptureKeyboard = false;
        bool wantCaptureMouse = false;
        bool wantTextInput = false;

        [[nodiscard]] bool uiClaimsInput() const noexcept {
            return wantCaptureKeyboard || wantCaptureMouse || wantTextInput;
        }
    };

    class ImGuiContent {
    public:
        virtual ~ImGuiContent() = default;
        virtual void draw() = 0;
    };

} // namespace lumin::render

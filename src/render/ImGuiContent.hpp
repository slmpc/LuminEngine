#pragma once

namespace lumin::render {

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

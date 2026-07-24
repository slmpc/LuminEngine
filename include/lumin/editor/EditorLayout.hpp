#pragma once

namespace lumin::editor {

    inline constexpr float EditorFullLayoutWidth = 1280.0f;
    inline constexpr float EditorFullLayoutHeight = 720.0f;
    inline constexpr float EditorFullLayoutWidthTolerance = 4.0f;
    inline constexpr float EditorFullLayoutHeightTolerance = 9.0f;

    enum class EditorLayoutMode {
        Full,
        Compact,
    };

    [[nodiscard]] constexpr EditorLayoutMode editorLayoutModeForExtent(float width, float height) noexcept {
        return width >= EditorFullLayoutWidth && height >= EditorFullLayoutHeight ? EditorLayoutMode::Full
                                                                                  : EditorLayoutMode::Compact;
    }

    [[nodiscard]] constexpr EditorLayoutMode editorLayoutModeForViewportSize(float width, float height) noexcept {
        return editorLayoutModeForExtent(width + EditorFullLayoutWidthTolerance,
                                         height + EditorFullLayoutHeightTolerance);
    }

    class EditorLayoutLifecycle {
    public:
        [[nodiscard]] bool update(const void* context, EditorLayoutMode mode, int schema) noexcept {
            if (context == nullptr) {
                return false;
            }
            const bool changed = !initialized_ || context_ != context || mode_ != mode || schema_ != schema;
            context_ = context;
            mode_ = mode;
            schema_ = schema;
            initialized_ = true;
            return changed;
        }

    private:
        const void* context_ = nullptr;
        EditorLayoutMode mode_ = EditorLayoutMode::Full;
        int schema_ = 0;
        bool initialized_ = false;
    };

} // namespace lumin::editor

#pragma once

#include "render/core/UiTextureId.hpp"

#include <cstdint>

namespace lumin::render {

    /** 可由 ImGui 窗口展示的渲染输出。尺寸使用物理像素。 */
    struct ImGuiViewportImage {
        /// Presentation 模块解析的稳定逻辑纹理 ID。
        core::UiTextureId textureId;
        /// 物理像素宽度。
        std::uint32_t width = 0;
        /// 物理像素高度。
        std::uint32_t height = 0;

        /// 返回逻辑纹理和尺寸是否都有效。
        [[nodiscard]] bool isValid() const noexcept {
            return textureId.isValid() && width != 0 && height != 0;
        }
    };

    /// 主线程 Dear ImGui backend 发布的输入捕获状态。
    struct ImGuiCaptureState {
        /// UI 是否请求键盘输入。
        bool wantCaptureKeyboard = false;
        /// UI 是否请求鼠标输入。
        bool wantCaptureMouse = false;
        /// UI 是否请求文本/IME 输入。
        bool wantTextInput = false;

        /// 返回 UI 是否请求至少一种输入通道。
        [[nodiscard]] bool uiClaimsInput() const noexcept {
            return wantCaptureKeyboard || wantCaptureMouse || wantTextInput;
        }
    };

    /// 渲染主线程 UI 内容适配器；Feature 不得持有或调用该接口。
    class ImGuiContent {
    public:
        /// 允许通过接口安全销毁 UI 内容。
        virtual ~ImGuiContent() = default;
        /// 在活动 Dear ImGui 帧内构建控件。
        virtual void draw() = 0;
    };

} // namespace lumin::render

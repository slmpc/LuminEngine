#pragma once

#include "render/RenderSettings.hpp"
#include "render/core/RenderSettingsStore.hpp"

#include <memory>

namespace lumin::render::editor {

    /**
     * @brief 将 Editor 面板的聚合编辑值适配到按 Feature 类型化的设置 Store。
     *
     * Adapter 只允许主线程访问，不包含 ImGui 代码。面板修改 `editable()` 返回的值后，调用 `snapshot()` 会按各 Feature
     * schema 校验并发布完全拥有的不可变快照。
     */
    class RenderSettingsPanelAdapter final {
    public:
        /** 使用内置 Feature 默认 schema 和默认值创建 adapter。 */
        RenderSettingsPanelAdapter();

        /** 等待内部 schema/store 完整销毁。 */
        ~RenderSettingsPanelAdapter();

        RenderSettingsPanelAdapter(const RenderSettingsPanelAdapter&) = delete;
        RenderSettingsPanelAdapter& operator=(const RenderSettingsPanelAdapter&) = delete;

        /** 返回 Editor 面板独占修改的兼容聚合视图。 */
        [[nodiscard]] RenderSettings& editable() noexcept;

        /** 返回当前兼容聚合视图的只读引用。 */
        [[nodiscard]] const RenderSettings& editable() const noexcept;

        /**
         * @brief 校验当前聚合值并生成 immutable typed snapshot。
         * @throws std::invalid_argument 任一 Feature 设置未通过 schema 校验时抛出。
         */
        [[nodiscard]] core::RenderSettingsSnapshot snapshot();

    private:
        struct State;
        std::unique_ptr<State> state_;
        RenderSettings editable_;
    };

} // namespace lumin::render::editor

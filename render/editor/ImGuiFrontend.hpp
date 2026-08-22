#pragma once

#include "render/core/UiDrawPacket.hpp"
#include "render/editor/ImGuiContent.hpp"

struct ImDrawData;

namespace lumin::platform {
    class Window;
}

namespace lumin::render {

    /**
     * @brief 在主线程独占 Dear ImGui context 和 SDL backend，并生成深拷贝 UI packet。
     *
     * 该类型不得在渲染线程调用。`finishFrame()` 返回的数据不含 ImGui 指针，可以移交给 latest-wins mailbox。
     */
    class ImGuiFrontend final {
    public:
        /// 构造未初始化前端。
        ImGuiFrontend() = default;

        /// 结束活动 UI 帧并释放 SDL backend 与 ImGui context。
        ~ImGuiFrontend();

        ImGuiFrontend(const ImGuiFrontend&) = delete;
        ImGuiFrontend& operator=(const ImGuiFrontend&) = delete;

        /**
         * @brief 在当前线程创建 ImGui context 并绑定窗口 SDL 事件。
         * @throws std::runtime_error SDL backend 或字体图集初始化失败时抛出。
         */
        void initialize(platform::Window& window);

        /// 幂等关闭前端；存在未完成帧时先调用 `ImGui::EndFrame()`。
        void shutdown() noexcept;

        /**
         * @brief 开始 UI 帧并立即调用可选内容适配器。
         * @throws std::logic_error 前端未初始化或已有活动帧时抛出。
         */
        void beginFrame(ImGuiContent* content = nullptr);

        /**
         * @brief 结束当前 ImGui 帧并深拷贝所有顶点、索引和命令。
         * @throws std::logic_error 当前没有活动帧时抛出。
         * @throws std::invalid_argument draw data 含任意 user callback 时抛出。
         */
        [[nodiscard]] core::UiDrawPacket finishFrame();

        /// 放弃当前 UI 帧；没有活动帧时不执行操作。
        void cancelFrame() noexcept;

        /// 返回当前 ImGui 输入捕获状态；未初始化时返回空状态。
        [[nodiscard]] ImGuiCaptureState captureState() const noexcept;

        /// 返回主线程深拷贝的字体图集；引用在下一次 `shutdown()` 前有效。
        [[nodiscard]] const core::UiFontAtlas& fontAtlas() const noexcept;

        /// 返回前端是否已初始化。
        [[nodiscard]] bool initialized() const noexcept;

        /// 返回当前是否存在尚未 finish/cancel 的 UI 帧。
        [[nodiscard]] bool frameActive() const noexcept;

        /**
         * @brief 将一个完整 `ImDrawData` 深拷贝为线程安全 packet。
         * @throws std::invalid_argument draw data 含任意非 reset-state user callback 时抛出。
         */
        [[nodiscard]] static core::UiDrawPacket buildDrawPacket(const ImDrawData& drawData);

    private:
        platform::Window* window_ = nullptr;
        core::UiFontAtlas fontAtlas_;
        bool contextCreated_ = false;
        bool sdlInitialized_ = false;
        bool initialized_ = false;
        bool frameActive_ = false;
    };

} // namespace lumin::render

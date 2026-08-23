#pragma once

#include "game/Game.hpp"
#include "render/editor/EditorLogic.hpp"
#include "scene/Camera.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lumin::core {

    /** 启动独立逻辑 Runtime 所需的配置。 */
    struct LogicRuntimeConfig {
        /** Lua 脚本查找根目录；由逻辑线程中的 `ScriptRuntime` 按值持有。 */
        std::filesystem::path scriptRoot;
        /** 可选启动脚本；仅在线程初始化期间加载一次。 */
        std::optional<std::filesystem::path> startupScript;
        /** 可选启动项目；打开失败时 Runtime 保持 Ready 并发布诊断。 */
        std::optional<std::filesystem::path> startupProject;
    };

    /** 渲染主线程发布给逻辑线程的最新游戏输入状态。 */
    struct LogicInputState {
        /** 游戏输入；空值表示当前帧不向 `Game` 派发输入。 */
        std::optional<game::GameInput> game;
    };

    /** 逻辑 Runtime 的稳定生命周期状态。 */
    enum class LogicRuntimeState {
        /** 逻辑对象正在工作线程构造。 */
        Starting,
        /** Runtime 已发布首份快照并可接收命令。 */
        Ready,
        /** 线程及其拥有的全部逻辑对象已销毁。 */
        Stopped,
        /** 初始化或 Tick 发生不可恢复异常。 */
        Failed,
    };

    /** 可跨线程读取的逻辑 Runtime 状态副本。 */
    struct LogicRuntimeStatus {
        /** 当前生命周期状态。 */
        LogicRuntimeState state = LogicRuntimeState::Starting;
        /** 已完成的固定逻辑 Tick 数。 */
        std::uint64_t completedTicks = 0;
        /** 执行开始时已经落后至少一个周期的 Tick 数。 */
        std::uint64_t missedDeadlines = 0;
        /** 当前生效的项目逻辑频率；无项目时为默认频率。 */
        std::uint32_t logicTickHz = project::DefaultLogicTickHz;
        /** 最近的启动或失败诊断；正常状态为空。 */
        std::string diagnostic;
    };

    /**
     * @brief 独占 Game、场景、项目和 Lua 的固定频率逻辑线程。
     *
     * 构造函数等待逻辑对象初始化及首份快照；析构会请求停止并 join。
     */
    class LogicRuntime final {
    public:
        /**
         * @brief 启动逻辑线程并等待首份快照。
         * @param config 脚本与启动项目配置，按值转移到逻辑线程。
         * @param game 由逻辑线程独占并在该线程销毁的游戏实例。
         * @throws std::invalid_argument `game` 为空时抛出。
         * @throws std::exception 逻辑线程初始化失败时在构造线程重新抛出。
         */
        LogicRuntime(LogicRuntimeConfig config, std::unique_ptr<game::Game> game);
        /** 请求停止、等待线程退出并销毁逻辑对象。 */
        ~LogicRuntime();

        LogicRuntime(const LogicRuntime&) = delete;
        LogicRuntime& operator=(const LogicRuntime&) = delete;

        /** 返回最近发布的不可变逻辑快照。 */
        [[nodiscard]] std::shared_ptr<const editor::EditorLogicSnapshot> snapshot() const noexcept;
        /** 排队一个由逻辑线程执行的 Editor 命令。 */
        [[nodiscard]] editor::EditorCommandId submit(editor::EditorLogicCommand command);
        /** 取走当前已经完成的命令结果。 */
        [[nodiscard]] std::vector<editor::EditorCommandResult> drainResults();
        /** 覆盖逻辑线程下一次固定 Tick 使用的最新游戏输入状态。 */
        void publishInput(LogicInputState input);
        /**
         * @brief 发布渲染主线程拥有的最新 Viewport Camera 镜像。
         * @param camera
         * 按值复制的相机状态；逻辑线程在下一次 Tick 或命令前消费。
         *
         *
         * 该调用不会唤醒逻辑线程，因而渲染帧率不会改变固定逻辑调度频率。
         */
        void publishCamera(scene::Camera camera);
        /** 返回线程安全的 Runtime 状态副本。 */
        [[nodiscard]] LogicRuntimeStatus status() const;
        /** 若逻辑线程失败则重新抛出原始异常。 */
        void rethrowIfFailed() const;
        /** 请求停止并等待逻辑对象在拥有线程销毁。 */
        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::core

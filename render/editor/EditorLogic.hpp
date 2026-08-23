#pragma once

#include "project/ProjectSession.hpp"
#include "render/world/RenderWorld.hpp"
#include "scene/Camera.hpp"
#include "scene/Level.hpp"
#include "scripting/ScriptRuntime.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lumin::editor {

    /** Editor 快照中的 Actor 值记录。 */
    struct EditorActorSnapshot {
        /** 逻辑场景中的稳定 Actor 句柄。 */
        scene::ActorHandle handle;
        /** Actor 显示名称的值副本。 */
        std::string name;
        /** 当前变换值。 */
        scene::Transform transform;
        /** 当前材质值。 */
        scene::Material material;
        /** Actor 绑定的模型句柄；无模型时无效。 */
        scene::ModelHandle modelHandle;
    };

    /** Editor 快照中的模型值记录。 */
    struct EditorModelSnapshot {
        /** 逻辑场景中的稳定模型句柄。 */
        scene::ModelHandle handle;
        /** 模型实例的值副本。 */
        scene::ModelInstance model;
        /** 可选拥有 Actor。 */
        std::optional<scene::ActorHandle> actor;
        /** 可选项目 Mesh 资产 ID。 */
        std::optional<project::AssetId> meshAsset;
    };

    /** Editor 使用的项目值快照。 */
    struct EditorProjectSnapshot {
        /** 当前是否打开项目。 */
        bool open = false;
        /** 当前项目是否包含未保存修改。 */
        bool dirty = false;
        /** 项目清单绝对路径；无项目时为空。 */
        std::filesystem::path projectFile;
        /** 项目根目录；无项目时为空。 */
        std::filesystem::path rootDirectory;
        /** 项目清单值副本。 */
        project::ProjectManifest manifest;
        /** 资产注册表值副本。 */
        project::AssetRegistry assets;
        /** Content Browser 使用的项目条目。 */
        std::vector<project::ProjectEntry> entries;
        /** 项目加载与同步诊断。 */
        std::vector<std::string> diagnostics;
        /** 已归一化的完整项目设置。 */
        project::ProjectSettings settings;
    };

    /**
     * @brief 逻辑线程发布给 Editor 和渲染主线程的不可变状态。
     *
     * 快照只含值与 renderer-owned 共享快照，不引用活动逻辑对象。
     */
    struct EditorLogicSnapshot {
        /** 每次发布快照时递增的单调修订号。 */
        std::uint64_t revision = 0;
        /** 生成该快照时已完成的逻辑 Tick 数。 */
        std::uint64_t logicTick = 0;
        /** 生成该快照前最后执行的 Editor 命令 ID。 */
        std::uint64_t lastAppliedCommand = 0;
        /** 与本快照其余字段一致的不可变渲染世界。 */
        render::world::RenderWorldSnapshotPtr renderWorld;
        /** 当前相机值副本。 */
        scene::Camera camera;
        /** 当前场景环境值副本。 */
        scene::SceneEnvironment environment;
        /** Actor 值记录。 */
        std::vector<EditorActorSnapshot> actors;
        /** 模型值记录。 */
        std::vector<EditorModelSnapshot> models;
        /** 已加载脚本信息。 */
        std::vector<scripting::ScriptInfo> scripts;
        /** 脚本诊断历史。 */
        std::vector<scripting::ScriptDiagnostic> scriptDiagnostics;
        /** Lua 控制台历史。 */
        std::vector<scripting::ConsoleHistoryEntry> consoleHistory;
        /** 当前项目值快照。 */
        EditorProjectSnapshot project;
        /** Runtime 启动或项目打开诊断。 */
        std::string diagnostic;

        /** 按稳定句柄查找 Actor。 */
        [[nodiscard]] const EditorActorSnapshot* findActor(scene::ActorHandle handle) const noexcept;
        /** 按稳定句柄查找模型。 */
        [[nodiscard]] const EditorModelSnapshot* findModel(scene::ModelHandle handle) const noexcept;
        /** 按稳定句柄查找脚本。 */
        [[nodiscard]] const scripting::ScriptInfo* findScript(scripting::ScriptHandle handle) const noexcept;
        /** 返回指定 Actor 的脚本，保持逻辑线程执行顺序。 */
        [[nodiscard]] std::vector<scripting::ScriptInfo> scriptsForActor(scene::ActorHandle actor) const;
    };

    /** 异步 Editor 命令 ID。 */
    using EditorCommandId = std::uint64_t;

    /** Editor 命令在逻辑线程完成后返回的值。 */
    struct EditorCommandOutcome {
        /** 命令是否成功。 */
        bool succeeded = true;
        /** 可选用户可见结果或错误消息。 */
        std::string message;
        /** 可选新建 Actor 句柄。 */
        std::optional<scene::ActorHandle> actor;
        /** 可选 Lua 执行结果。 */
        std::optional<scripting::ScriptResult> scriptResult;
        /** 可选批量脚本重载结果。 */
        std::vector<scripting::ScriptReloadResult> reloadResults;
    };

    /**
     * @brief 在逻辑线程独占对象上执行的一次完全拥有命令。
     *
     * 闭包必须只捕获可独立拥有的值，不得捕获 Editor、ImGui 或 SDL 指针。
     */
    using EditorLogicCommand = std::function<EditorCommandOutcome(scene::Level&, scene::Camera&,
                                                                  scripting::ScriptRuntime&, project::ProjectSession&)>;

    /** 已在逻辑线程结束的 Editor 命令结果。 */
    struct EditorCommandResult {
        /** 对应提交命令的单调 ID。 */
        EditorCommandId id = 0;
        /** 逻辑线程完成后的按值结果。 */
        EditorCommandOutcome outcome;
    };

    /**
     * @brief Editor 与逻辑 Runtime 之间的线程安全服务入口。
     *
     * 回调由 Application 注入，只能在 Application 与 LogicRuntime 生命周期内调用。
     */
    struct EditorLogicServices {
        /** 返回当前渲染帧固定使用的不可变快照。 */
        std::function<std::shared_ptr<const EditorLogicSnapshot>()> snapshot;
        /** 将完全拥有的命令排入逻辑线程。 */
        std::function<EditorCommandId(EditorLogicCommand)> submit;
        /** 取走已经完成且尚未消费的命令结果。 */
        std::function<std::vector<EditorCommandResult>()> drainResults;
    };

} // namespace lumin::editor

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include "config/EngineSettings.hpp"
#include "project/ProjectSession.hpp"
#include "render/RenderSettings.hpp"
#include "render/editor/EditorLogic.hpp"
#include "render/editor/ImGuiContent.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include "scene/Camera.hpp"
#include "scene/Level.hpp"
#include "scripting/ScriptRuntime.hpp"

namespace lumin::editor {

    enum class SelectionState {
        Empty,
        Actor,
        Model,
        Script,
        Stale,
    };

    struct ConsoleEntry {
        scripting::ScriptSeverity severity = scripting::ScriptSeverity::Info;
        scripting::ScriptPhase phase = scripting::ScriptPhase::Console;
        scripting::ScriptHandle script;
        std::string source;
        std::string message;
        enum class Kind {
            Diagnostic,
            CommandResult,
        } kind = Kind::Diagnostic;
    };

    using BackendInfoProvider = std::function<render::gi::BackendInfo()>;
    using ViewportImageProvider = std::function<render::ImGuiViewportImage()>;
    /** 返回渲染主线程最近统计的交换链呈现帧率。 */
    using FrameRateProvider = std::function<float()>;
    using DialogResultCallback = std::function<void(std::vector<std::filesystem::path>)>;

    struct EditorDialogServices {
        std::function<void(DialogResultCallback)> openProject;
        std::function<void(DialogResultCallback)> openFolder;
    };

    using SaveEngineSettingsCallback = std::function<bool(const config::EngineSettings& settings, std::string& error)>;

    struct EditorSettingsServices {
        config::EngineSettings settings;
        SaveEngineSettingsCallback save;
    };

    struct ViewportInteractionState {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool hovered = false;
        bool focused = false;

        [[nodiscard]] bool hasRenderableExtent() const noexcept {
            return width != 0 && height != 0;
        }
    };

    class Editor final : public render::ImGuiContent {
    public:
        /**
         * @brief 创建通过不可变快照和异步命令访问逻辑状态的 Editor UI。
         * @param logic
         * 线程安全的逻辑快照、命令提交与结果服务。
         * @param settings 可写渲染设置，必须覆盖 Editor 生命周期。

         * * @param backendInfo 当前 GI backend 状态 provider。
         * @param viewportImage 当前 Viewport 输出纹理
         * provider。
         * @param dialogs 平台文件对话框服务。
         * @param settingsServices Editor
         * 持久化设置服务。
         * @param frameRate 当前交换链呈现帧率 provider；为空时显示零。

         */
        Editor(EditorLogicServices logic, render::RenderSettings& settings, BackendInfoProvider backendInfo,
               ViewportImageProvider viewportImage = {}, EditorDialogServices dialogs = {},
               EditorSettingsServices settingsServices = {}, FrameRateProvider frameRate = {});
        ~Editor() override;

        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;
        Editor(Editor&&) noexcept;
        Editor& operator=(Editor&&) noexcept;

        void draw() override;

        /** 返回最近一次 UI 构建得到的 Viewport 内容区状态。 */
        [[nodiscard]] ViewportInteractionState viewportInteraction() const noexcept;

        [[nodiscard]] SelectionState selectionState() const noexcept;
        [[nodiscard]] std::optional<scene::ActorHandle> selectedActor() const noexcept;
        [[nodiscard]] std::optional<scene::ModelHandle> selectedModel() const noexcept;
        [[nodiscard]] std::optional<scripting::ScriptHandle> selectedScript() const noexcept;
        bool selectActor(scene::ActorHandle actor);
        bool selectModel(scene::ModelHandle model);
        bool selectScript(scripting::ScriptHandle script);
        void clearSelection() noexcept;
        void synchronizeSelection();

        bool setSelectedTransform(const scene::Transform& transform);
        bool setSelectedMaterial(const scene::Material& material);

        void setCameraSpeed(float speed);
        void setCameraPosition(const glm::vec3& position);
        void setDirectLightingEnabled(bool enabled) noexcept;
        void setShadowsEnabled(bool enabled) noexcept;
        void setGlobalIlluminationMode(render::GlobalIlluminationMode mode) noexcept;
        void setSsaoEnabled(bool enabled) noexcept;
        void setAmbientOcclusionMode(render::AmbientOcclusionMode mode) noexcept;
        void setAmbientOcclusionRadius(float radius) noexcept;
        void setAmbientOcclusionStrength(float strength) noexcept;
        void setAmbientOcclusionBias(float bias) noexcept;
        void setSharcEnabled(bool enabled) noexcept;
        void setNrdEnabled(bool enabled) noexcept;
        void setCsmSplitLambda(float splitLambda) noexcept;
        void setCsmMaxDistance(float maxDistance) noexcept;
        void setTaaEnabled(bool enabled) noexcept;
        void setExposure(float exposure) noexcept;
        void setSunDirection(const glm::vec3& direction);

        [[nodiscard]] scripting::ScriptResult executeCommand(std::string_view command);
        [[nodiscard]] const std::vector<ConsoleEntry>& consoleEntries() const noexcept;
        void synchronizeConsole();
        void clearDiagnostics();
        void clearCommandHistory();
        [[nodiscard]] std::string commandHistoryPrevious(std::string_view draft);
        [[nodiscard]] std::string commandHistoryNext();
        [[nodiscard]] std::vector<scripting::ScriptReloadResult> reloadChangedScripts();
        bool openProject(const std::filesystem::path& path);
        [[nodiscard]] bool exitRequested() const noexcept;
        void requestExit();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::editor

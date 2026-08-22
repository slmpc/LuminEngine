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

#include "project/ProjectSession.hpp"
#include "render/RenderSettings.hpp"
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
    using DialogResultCallback = std::function<void(std::vector<std::filesystem::path>)>;

    struct EditorDialogServices {
        std::function<void(DialogResultCallback)> openProject;
        std::function<void(DialogResultCallback)> openFolder;
        std::function<std::vector<std::filesystem::path>()> loadRecentProjects;
        std::function<void(const std::vector<std::filesystem::path>&)> saveRecentProjects;
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
        Editor(scene::Level& level, scene::Camera& camera, render::RenderSettings& settings,
               scripting::ScriptRuntime& scripts, BackendInfoProvider backendInfo,
               ViewportImageProvider viewportImage = {}, project::ProjectSession* project = nullptr,
               EditorDialogServices dialogs = {});
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

        void setCameraSpeed(float speed) noexcept;
        void setCameraPosition(const glm::vec3& position) noexcept;
        void setDirectLightingEnabled(bool enabled) noexcept;
        void setShadowsEnabled(bool enabled) noexcept;
        void setGlobalIlluminationMode(render::GlobalIlluminationMode mode) noexcept;
        void setSsaoEnabled(bool enabled) noexcept;
        void setSharcEnabled(bool enabled) noexcept;
        void setNrdEnabled(bool enabled) noexcept;
        void setCsmSplitLambda(float splitLambda) noexcept;
        void setCsmMaxDistance(float maxDistance) noexcept;
        void setTaaEnabled(bool enabled) noexcept;
        void setExposure(float exposure) noexcept;
        void setSunDirection(const glm::vec3& direction) noexcept;

        [[nodiscard]] scripting::ScriptResult executeCommand(std::string_view command);
        [[nodiscard]] const std::vector<ConsoleEntry>& consoleEntries() const noexcept;
        void synchronizeConsole();
        void clearDiagnostics();
        void clearCommandHistory();
        [[nodiscard]] std::string commandHistoryPrevious(std::string_view draft);
        [[nodiscard]] std::string commandHistoryNext();
        [[nodiscard]] std::vector<scripting::ScriptReloadResult> reloadChangedScripts();
        [[nodiscard]] bool exitRequested() const noexcept;
        void requestExit();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::editor

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include "render/ImGuiContent.hpp"
#include "render/RenderSettings.hpp"
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

    class Editor final : public render::ImGuiContent {
    public:
        Editor(scene::Level& level, scene::Camera& camera, render::RenderSettings& settings,
               scripting::ScriptRuntime& scripts, BackendInfoProvider backendInfo);
        ~Editor() override;

        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;
        Editor(Editor&&) noexcept;
        Editor& operator=(Editor&&) noexcept;

        void draw() override;

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
        void setGlobalIlluminationEnabled(bool enabled) noexcept;
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

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::editor

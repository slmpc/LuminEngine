#include "editor/Editor.hpp"

#include "EditorStyle.hpp"
#include "editor/EditorLayout.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

namespace lumin::editor {
    namespace {

        const char* severityName(scripting::ScriptSeverity severity) {
            switch (severity) {
            case scripting::ScriptSeverity::Info:
                return "Info";
            case scripting::ScriptSeverity::Warning:
                return "Warning";
            case scripting::ScriptSeverity::Error:
                return "Error";
            }
            return "Unknown";
        }

        const char* phaseName(scripting::ScriptPhase phase) {
            switch (phase) {
            case scripting::ScriptPhase::Load:
                return "Load";
            case scripting::ScriptPhase::Spawn:
                return "Spawn";
            case scripting::ScriptPhase::Tick:
                return "Tick";
            case scripting::ScriptPhase::Destroy:
                return "Destroy";
            case scripting::ScriptPhase::Reload:
                return "Reload";
            case scripting::ScriptPhase::Console:
                return "Console";
            }
            return "Unknown";
        }

        bool modelAlive(const scene::Level& level, scene::ModelHandle handle) {
            try {
                static_cast<void>(level.model(handle));
                return true;
            } catch (const std::out_of_range&) {
                return false;
            }
        }

        void section(const char* title) {
            ImGui::Spacing();
            ImGui::SeparatorText(title);
        }

        void propertyLabel(const char* label) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(style::PropertyLabelWidth);
            ImGui::SetNextItemWidth(-style::Space1);
        }

    } // namespace

    struct Editor::Impl {
        Impl(scene::Level& levelValue, scene::Camera& cameraValue, render::RenderSettings& settingsValue,
             scripting::ScriptRuntime& scriptsValue, BackendInfoProvider backendInfoValue)
            : level(levelValue), camera(cameraValue), settings(settingsValue), scripts(scriptsValue),
              backendInfo(std::move(backendInfoValue)) {
        }

        scene::Level& level;
        scene::Camera& camera;
        render::RenderSettings& settings;
        scripting::ScriptRuntime& scripts;
        BackendInfoProvider backendInfo;
        SelectionState selection = SelectionState::Empty;
        std::optional<scene::ActorHandle> actor;
        std::optional<scene::ModelHandle> model;
        std::optional<scripting::ScriptHandle> script;
        std::vector<ConsoleEntry> console;
        std::uint64_t lastDiagnosticSequence = 0;
        std::array<char, 512> command{};
        std::array<char, 128> search{};
        std::optional<std::size_t> historyIndex;
        std::string historyDraft;
        bool showInfo = true;
        bool showWarnings = true;
        bool showErrors = true;
        EditorLayoutLifecycle layoutLifecycle;

        void markStale(std::string message) {
            selection = SelectionState::Stale;
            actor.reset();
            model.reset();
            script.reset();
            console.push_back({scripting::ScriptSeverity::Warning,
                               scripting::ScriptPhase::Console,
                               {},
                               "Editor",
                               std::move(message)});
        }

        void synchronizeDiagnostics() {
            for (const scripting::ScriptDiagnostic& diagnostic : scripts.diagnostics(lastDiagnosticSequence)) {
                console.push_back({diagnostic.severity, diagnostic.phase, diagnostic.script,
                                   diagnostic.source.generic_string(), diagnostic.message});
                lastDiagnosticSequence = std::max(lastDiagnosticSequence, diagnostic.sequence);
            }
        }

        scripting::ScriptResult executeCommand(std::string_view source) {
            scripting::ScriptResult result = scripts.execute(source);
            historyIndex.reset();
            historyDraft.clear();
            if (result.succeeded) {
                if (result.values.empty()) {
                    console.push_back({scripting::ScriptSeverity::Info,
                                       scripting::ScriptPhase::Console,
                                       {},
                                       "<console>",
                                       "Command completed",
                                       ConsoleEntry::Kind::CommandResult});
                } else {
                    for (const std::string& value : result.values) {
                        console.push_back({scripting::ScriptSeverity::Info,
                                           scripting::ScriptPhase::Console,
                                           {},
                                           "<console>",
                                           value,
                                           ConsoleEntry::Kind::CommandResult});
                    }
                }
            } else {
                synchronizeDiagnostics();
            }
            return result;
        }

        void clearDiagnostics() {
            scripts.clearDiagnostics();
            std::erase_if(console, [](const ConsoleEntry& entry) {
                return entry.kind == ConsoleEntry::Kind::Diagnostic;
            });
        }

        void clearCommandHistory() {
            scripts.clearConsoleHistory();
            historyIndex.reset();
            historyDraft.clear();
        }

        std::string commandHistoryPrevious(std::string_view draft) {
            const std::vector<scripting::ConsoleHistoryEntry> history = scripts.consoleHistory();
            if (history.empty()) {
                return std::string{draft};
            }
            if (!historyIndex.has_value()) {
                historyDraft = draft;
                historyIndex = history.size() - 1;
            } else if (*historyIndex > 0) {
                --*historyIndex;
            }
            return history[*historyIndex].command;
        }

        std::string commandHistoryNext() {
            if (!historyIndex.has_value()) {
                return historyDraft;
            }
            const std::vector<scripting::ConsoleHistoryEntry> history = scripts.consoleHistory();
            if (*historyIndex + 1 < history.size()) {
                ++*historyIndex;
                return history[*historyIndex].command;
            }
            historyIndex.reset();
            return historyDraft;
        }

        std::vector<scripting::ScriptReloadResult> reloadChangedScripts() {
            std::vector<scripting::ScriptReloadResult> results = scripts.reloadChanged();
            for (const scripting::ScriptReloadResult& reload : results) {
                if (reload.result.succeeded) {
                    console.push_back({scripting::ScriptSeverity::Info,
                                       scripting::ScriptPhase::Reload,
                                       reload.script,
                                       {},
                                       "Reloaded"});
                } else if (reload.result.error.has_value()) {
                    console.push_back({scripting::ScriptSeverity::Error, reload.result.error->phase, reload.script,
                                       reload.result.error->source.generic_string(), reload.result.error->message});
                }
            }
            return results;
        }

        void buildLayout(ImGuiID dockspace, const ImGuiViewport& viewport) {
            if (viewport.WorkSize.x <= 0.0f || viewport.WorkSize.y <= 0.0f) {
                return;
            }
            const EditorLayoutMode mode = editorLayoutModeForViewportSize(viewport.Size.x, viewport.Size.y);
            const bool transition = layoutLifecycle.update(ImGui::GetCurrentContext(), mode, style::LayoutSchema);
            if (!transition && ImGui::DockBuilderGetNode(dockspace) != nullptr) {
                return;
            }
            if (ImGui::DockBuilderGetNode(dockspace) != nullptr) {
                ImGui::DockBuilderRemoveNode(dockspace);
            }
            ImGui::DockBuilderAddNode(dockspace,
                                      ImGuiDockNodeFlags_DockSpace |
                                          static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_PassthruCentralNode));
            ImGui::DockBuilderSetNodePos(dockspace, viewport.WorkPos);
            ImGui::DockBuilderSetNodeSize(dockspace, viewport.WorkSize);

            ImGuiID center = dockspace;
            ImGuiID left = 0;
            ImGuiID right = 0;
            ImGuiID bottom = 0;
            if (mode == EditorLayoutMode::Compact) {
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, style::HierarchyRatio, &left, &center);
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, style::ConsoleRatio, &bottom, &center);
                ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
                ImGui::DockBuilderDockWindow("Inspector", left);
                ImGui::DockBuilderDockWindow("Render / GI", bottom);
                ImGui::DockBuilderDockWindow("Script Console", bottom);
            } else {
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, style::HierarchyRatio, &left, &center);
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, style::PropertiesRatio, &right, &center);
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, style::ConsoleRatio, &bottom, &center);
                ImGuiID inspector = 0;
                ImGuiID renderGi = 0;
                ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, style::InspectorRatio, &inspector, &renderGi);
                ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
                ImGui::DockBuilderDockWindow("Inspector", inspector);
                ImGui::DockBuilderDockWindow("Render / GI", renderGi);
                ImGui::DockBuilderDockWindow("Script Console", bottom);
            }
            ImGui::DockBuilderFinish(dockspace);
        }

        static int commandHistoryCallback(ImGuiInputTextCallbackData* data) {
            auto& self = *static_cast<Impl*>(data->UserData);
            const std::string replacement = data->EventKey == ImGuiKey_UpArrow
                                                ? self.commandHistoryPrevious(std::string_view{data->Buf})
                                                : self.commandHistoryNext();
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, replacement.c_str());
            return 0;
        }

        void drawHierarchy() {
            ImGui::Begin("Scene Hierarchy");
            for (const scene::ActorHandle handle : level.actorHandles()) {
                char label[64]{};
                std::snprintf(label, sizeof(label), "Actor %u:%u", handle.index, handle.generation);
                const bool selected = actor == handle;
                ImGui::PushID(static_cast<int>(handle.index));
                ImGui::PushID(static_cast<int>(handle.generation));
                if (ImGui::Selectable(label, selected)) {
                    selectActor(handle);
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            for (const scene::ModelHandle handle : level.modelHandles()) {
                char label[64]{};
                std::snprintf(label, sizeof(label), "Model %u:%u", handle.index, handle.generation);
                const bool selected = model == handle;
                ImGui::PushID(static_cast<int>(handle.index));
                ImGui::PushID(static_cast<int>(handle.generation));
                if (ImGui::Selectable(label, selected)) {
                    selectModel(handle);
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            for (const scripting::ScriptInfo& info : scripts.scripts()) {
                const std::string label =
                    info.source.filename().generic_string() + "##script" + std::to_string(info.handle.value);
                if (ImGui::Selectable(label.c_str(), script == info.handle)) {
                    selectScript(info.handle);
                }
            }
            if (level.actorHandles().empty() && level.modelHandles().empty() && scripts.scripts().empty()) {
                ImGui::TextDisabled("No actors, models, or scripts");
            }
            ImGui::End();
        }

        void editTransform(scene::Transform transform) {
            bool changed = false;
            propertyLabel("Position");
            changed |= ImGui::DragFloat3("##position", &transform.position.x, 0.05f);
            propertyLabel("Rotation");
            changed |= ImGui::DragFloat3("##rotation", &transform.rotationDegrees.x, 0.25f);
            propertyLabel("Scale");
            changed |= ImGui::DragFloat3("##scale", &transform.scale.x, 0.01f, 0.001f, 1000.0f);
            if (changed) {
                setSelectedTransform(transform);
            }
        }

        void editMaterial(scene::Material material) {
            bool changed = false;
            constexpr const char* surfaceModels[] = {"Metallic-Roughness", "Blinn-Phong"};
            int surfaceModel = static_cast<int>(material.surfaceModel);
            propertyLabel("Surface model");
            if (ImGui::Combo("##surfaceModel", &surfaceModel, surfaceModels, std::size(surfaceModels))) {
                material.surfaceModel = static_cast<scene::SurfaceModel>(surfaceModel);
                changed = true;
            }
            propertyLabel("Albedo");
            changed |= ImGui::ColorEdit3("##albedo", &material.albedo.x);
            if (material.surfaceModel == scene::SurfaceModel::MetallicRoughness) {
                propertyLabel("Roughness");
                changed |= ImGui::SliderFloat("##roughness", &material.metallicRoughness.roughness, 0.0f, 1.0f);
                propertyLabel("Metallic");
                changed |= ImGui::SliderFloat("##metallic", &material.metallicRoughness.metallic, 0.0f, 1.0f);
            } else {
                propertyLabel("Specular");
                changed |= ImGui::ColorEdit3("##specular", &material.blinnPhong.specularColor.x);
                propertyLabel("Shininess");
                changed |= ImGui::SliderFloat("##shininess", &material.blinnPhong.shininess, 1.0f, 512.0f, "%.0f",
                                              ImGuiSliderFlags_Logarithmic);
            }
            propertyLabel("Texture scale");
            changed |= ImGui::DragFloat("##textureScale", &material.textureScale, 0.05f, 0.01f, 100.0f);
            if (changed) {
                setSelectedMaterial(material);
            }
        }

        void drawInspector() {
            ImGui::Begin("Inspector");
            if (selection == SelectionState::Stale) {
                ImGui::TextUnformatted("Selection became stale and was cleared.");
            } else if (actor.has_value()) {
                scene::Actor* selected = level.actor(*actor);
                if (selected != nullptr) {
                    section("Transform");
                    editTransform(selected->transform());
                    section("Material");
                    editMaterial(selected->material());
                }
            } else if (model.has_value()) {
                const scene::ModelInstance& selected = level.model(*model);
                section("Transform");
                editTransform(selected.transform);
                section("Material");
                editMaterial(selected.material);
            } else if (script.has_value()) {
                const std::optional<scripting::ScriptInfo> selected = scripts.script(*script);
                if (selected.has_value()) {
                    ImGui::Text("Handle: %llu", static_cast<unsigned long long>(selected->handle.value));
                    ImGui::TextWrapped("Source: %s", selected->source.generic_string().c_str());
                    ImGui::Text("Revision: %llu", static_cast<unsigned long long>(selected->revision));
                }
            } else {
                ImGui::TextDisabled("Select an actor, model, or script to inspect it.");
            }
            ImGui::End();
        }

        void drawViewport(ImGuiID dockspace, const ImGuiViewport& viewport) {
            constexpr ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
            const ImGuiDockNode* center = ImGui::DockBuilderGetCentralNode(dockspace);
            ImGui::SetNextWindowPos(center != nullptr ? center->Pos : viewport.Pos);
            ImGui::SetNextWindowSize(center != nullptr ? center->Size : viewport.Size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {style::Space0, style::Space0});
            ImGui::Begin("Viewport", nullptr, flags);
            ImGui::SetCursorPos({style::Space2, style::Space2});
            ImGui::TextDisabled("Viewport");
            ImGui::End();
            ImGui::PopStyleVar();
        }

        void drawRenderSettings() {
            ImGui::Begin("Render / GI");
            section("Camera");
            float speed = camera.moveSpeed();
            propertyLabel("Move speed");
            if (ImGui::DragFloat("##cameraSpeed", &speed, 0.05f, 0.1f, 20.0f)) {
                camera.setMoveSpeed(speed);
            }
            glm::vec3 position = camera.position();
            propertyLabel("Position");
            if (ImGui::DragFloat3("##cameraPosition", &position.x, 0.05f)) {
                camera.setPosition(position);
                camera.markCut();
            }
            section("Direct Lighting");
            ImGui::Checkbox("Enabled##directLighting", &settings.directLighting.enabled);
            section("Shadows");
            ImGui::Checkbox("Cascaded shadows", &settings.shadows.enabled);
            section("Global Illumination");
            ImGui::Checkbox("Enabled##gi", &settings.globalIllumination.enabled);
            const render::gi::BackendInfo backend = backendInfo();
            ImGui::Text("Backend: %.*s", static_cast<int>(backend.name.size()), backend.name.data());
            ImGui::Text("Temporal history: %s", backend.temporal ? "Supported" : "Not supported");
            ImGui::Text("Hardware ray tracing: %s", backend.hardwareRayTracing ? "Supported" : "Not supported");
            section("Temporal AA");
            ImGui::Checkbox("Enabled##taa", &settings.temporalAa.enabled);
            section("Tonemap");
            propertyLabel("Exposure");
            ImGui::SliderFloat("##exposure", &settings.toneMapping.exposure, 0.1f, 4.0f);
            section("Lighting");
            propertyLabel("Sun direction");
            scene::DirectionalLight sun = level.environment().sun;
            if (ImGui::SliderFloat3("##sunDirection", &sun.direction.x, -1.0f, 1.0f)) {
                level.setSun(sun);
            }
            ImGui::End();
        }

        bool visible(const ConsoleEntry& entry) const {
            const bool severityVisible = (entry.severity == scripting::ScriptSeverity::Info && showInfo) ||
                                         (entry.severity == scripting::ScriptSeverity::Warning && showWarnings) ||
                                         (entry.severity == scripting::ScriptSeverity::Error && showErrors);
            if (!severityVisible) {
                return false;
            }
            const std::string_view query{search.data()};
            return query.empty() || entry.message.find(query) != std::string::npos ||
                   entry.source.find(query) != std::string::npos;
        }

        void drawConsole() {
            synchronizeDiagnostics();
            ImGui::Begin("Script Console");
            ImGui::Checkbox("Info", &showInfo);
            ImGui::SameLine();
            ImGui::Checkbox("Warning", &showWarnings);
            ImGui::SameLine();
            ImGui::Checkbox("Error", &showErrors);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputTextWithHint("##search", "Filter", search.data(), search.size());
            ImGui::SameLine();
            if (ImGui::Button("Clear Diagnostics")) {
                clearDiagnostics();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear History")) {
                clearCommandHistory();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload Changed")) {
                reloadChangedScripts();
            }

            const float inputHeight = ImGui::GetFrameHeightWithSpacing();
            if (ImGui::BeginChild("ConsoleMessages", {0.0f, -inputHeight}, false,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const ConsoleEntry& entry : console) {
                    if (visible(entry)) {
                        ImGui::TextWrapped("[%s] [%s] %s%s%s", severityName(entry.severity), phaseName(entry.phase),
                                           entry.source.c_str(), entry.source.empty() ? "" : ": ",
                                           entry.message.c_str());
                    }
                }
                if (console.empty()) {
                    ImGui::TextDisabled("No script diagnostics or command results.");
                }
            }
            ImGui::EndChild();
            ImGui::SetNextItemWidth(-style::Space1);
            if (ImGui::InputTextWithHint("##command", "Lua command", command.data(), command.size(),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                         commandHistoryCallback, this)) {
                if (command.front() != '\0') {
                    executeCommand(command.data());
                    command.fill('\0');
                    ImGui::SetKeyboardFocusHere(-1);
                }
            }
            ImGui::End();
        }

        bool selectActor(scene::ActorHandle handle) {
            if (!level.isActorAlive(handle)) {
                return false;
            }
            selection = SelectionState::Actor;
            actor = handle;
            model.reset();
            script.reset();
            return true;
        }

        bool selectModel(scene::ModelHandle handle) {
            if (!modelAlive(level, handle)) {
                return false;
            }
            selection = SelectionState::Model;
            model = handle;
            actor.reset();
            script.reset();
            return true;
        }

        bool selectScript(scripting::ScriptHandle handle) {
            if (!scripts.script(handle).has_value()) {
                return false;
            }
            selection = SelectionState::Script;
            script = handle;
            actor.reset();
            model.reset();
            return true;
        }

        bool setSelectedTransform(const scene::Transform& transform) {
            if (actor.has_value()) {
                scene::Actor* selected = level.actor(*actor);
                if (selected == nullptr) {
                    markStale("Selected actor is no longer alive.");
                    return false;
                }
                selected->setTransform(transform);
                return true;
            }
            if (model.has_value() && level.setModelTransform(*model, transform)) {
                return true;
            }
            if (model.has_value()) {
                markStale("Selected model is no longer alive.");
            }
            return false;
        }

        bool setSelectedMaterial(const scene::Material& material) {
            if (actor.has_value()) {
                scene::Actor* selected = level.actor(*actor);
                if (selected == nullptr) {
                    markStale("Selected actor is no longer alive.");
                    return false;
                }
                selected->setMaterial(material);
                return true;
            }
            if (model.has_value() && level.setModelMaterial(*model, material)) {
                return true;
            }
            if (model.has_value()) {
                markStale("Selected model is no longer alive.");
            }
            return false;
        }
    };

    Editor::Editor(scene::Level& level, scene::Camera& camera, render::RenderSettings& settings,
                   scripting::ScriptRuntime& scripts, BackendInfoProvider backendInfo)
        : impl_(std::make_unique<Impl>(level, camera, settings, scripts, std::move(backendInfo))) {
    }

    Editor::~Editor() = default;
    Editor::Editor(Editor&&) noexcept = default;
    Editor& Editor::operator=(Editor&&) noexcept = default;

    void Editor::draw() {
        style::apply();
        synchronizeSelection();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImGuiID dockspace = ImGui::GetID("LuminEditorDockspace");
        impl_->buildLayout(dockspace, *viewport);
        ImGui::DockSpaceOverViewport(dockspace, viewport, ImGuiDockNodeFlags_PassthruCentralNode);
        impl_->drawHierarchy();
        impl_->drawInspector();
        impl_->drawViewport(dockspace, *viewport);
        impl_->drawRenderSettings();
        impl_->drawConsole();
    }

    SelectionState Editor::selectionState() const noexcept {
        return impl_->selection;
    }

    std::optional<scene::ActorHandle> Editor::selectedActor() const noexcept {
        return impl_->actor;
    }

    std::optional<scene::ModelHandle> Editor::selectedModel() const noexcept {
        return impl_->model;
    }

    std::optional<scripting::ScriptHandle> Editor::selectedScript() const noexcept {
        return impl_->script;
    }

    bool Editor::selectActor(scene::ActorHandle actor) {
        return impl_->selectActor(actor);
    }

    bool Editor::selectModel(scene::ModelHandle model) {
        return impl_->selectModel(model);
    }

    bool Editor::selectScript(scripting::ScriptHandle script) {
        return impl_->selectScript(script);
    }

    void Editor::clearSelection() noexcept {
        impl_->selection = SelectionState::Empty;
        impl_->actor.reset();
        impl_->model.reset();
        impl_->script.reset();
    }

    void Editor::synchronizeSelection() {
        if (impl_->actor.has_value() && !impl_->level.isActorAlive(*impl_->actor)) {
            impl_->markStale("Selected actor is no longer alive.");
        } else if (impl_->model.has_value() && !modelAlive(impl_->level, *impl_->model)) {
            impl_->markStale("Selected model is no longer alive.");
        } else if (impl_->script.has_value() && !impl_->scripts.script(*impl_->script).has_value()) {
            impl_->markStale("Selected script is no longer loaded.");
        }
    }

    bool Editor::setSelectedTransform(const scene::Transform& transform) {
        return impl_->setSelectedTransform(transform);
    }

    bool Editor::setSelectedMaterial(const scene::Material& material) {
        return impl_->setSelectedMaterial(material);
    }

    void Editor::setCameraSpeed(float speed) noexcept {
        impl_->camera.setMoveSpeed(speed);
    }

    void Editor::setCameraPosition(const glm::vec3& position) noexcept {
        impl_->camera.setPosition(position);
        impl_->camera.markCut();
    }

    void Editor::setDirectLightingEnabled(bool enabled) noexcept {
        impl_->settings.directLighting.enabled = enabled;
    }

    void Editor::setShadowsEnabled(bool enabled) noexcept {
        impl_->settings.shadows.enabled = enabled;
    }

    void Editor::setGlobalIlluminationEnabled(bool enabled) noexcept {
        impl_->settings.globalIllumination.enabled = enabled;
    }

    void Editor::setTaaEnabled(bool enabled) noexcept {
        impl_->settings.temporalAa.enabled = enabled;
    }

    void Editor::setExposure(float exposure) noexcept {
        impl_->settings.toneMapping.exposure = exposure;
    }

    void Editor::setSunDirection(const glm::vec3& direction) noexcept {
        scene::DirectionalLight sun = impl_->level.environment().sun;
        sun.direction = direction;
        impl_->level.setSun(sun);
    }

    scripting::ScriptResult Editor::executeCommand(std::string_view command) {
        return impl_->executeCommand(command);
    }

    const std::vector<ConsoleEntry>& Editor::consoleEntries() const noexcept {
        return impl_->console;
    }

    void Editor::synchronizeConsole() {
        impl_->synchronizeDiagnostics();
    }

    void Editor::clearDiagnostics() {
        impl_->clearDiagnostics();
    }

    void Editor::clearCommandHistory() {
        impl_->clearCommandHistory();
    }

    std::string Editor::commandHistoryPrevious(std::string_view draft) {
        return impl_->commandHistoryPrevious(draft);
    }

    std::string Editor::commandHistoryNext() {
        return impl_->commandHistoryNext();
    }

    std::vector<scripting::ScriptReloadResult> Editor::reloadChangedScripts() {
        return impl_->reloadChangedScripts();
    }

} // namespace lumin::editor

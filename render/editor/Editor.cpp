#include "render/editor/Editor.hpp"

#include "EditorStyle.hpp"
#include "render/editor/EditorLayout.hpp"
#include "render/editor/ViewportPicking.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

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
             scripting::ScriptRuntime& scriptsValue, BackendInfoProvider backendInfoValue,
             ViewportImageProvider viewportImageValue, project::ProjectSession* projectValue,
             EditorDialogServices dialogsValue)
            : level(levelValue), camera(cameraValue), settings(settingsValue), scripts(scriptsValue),
              backendInfo(std::move(backendInfoValue)), viewportImage(std::move(viewportImageValue)),
              projectSession(projectValue), dialogs(std::move(dialogsValue)) {
            if (dialogs.loadRecentProjects) {
                recentProjects = dialogs.loadRecentProjects();
                if (recentProjects.size() > 10) {
                    recentProjects.resize(10);
                }
            }
        }

        scene::Level& level;
        scene::Camera& camera;
        render::RenderSettings& settings;
        scripting::ScriptRuntime& scripts;
        BackendInfoProvider backendInfo;
        ViewportImageProvider viewportImage;
        project::ProjectSession* projectSession = nullptr;
        EditorDialogServices dialogs;
        ViewportInteractionState viewportInteraction;
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
        std::array<char, 128> projectName{};
        std::array<char, 128> contentSearch{};
        std::array<char, 128> renameAssetName{};
        std::optional<project::AssetId> renameAssetId;
        std::filesystem::path newProjectLocation;
        std::vector<std::filesystem::path> importSources;
        project::ImportConflictPolicy importConflict = project::ImportConflictPolicy::Rename;
        std::vector<std::filesystem::path> recentProjects;
        std::string statusMessage;
        std::function<void()> pendingDestructiveAction;
        bool showNewProject = false;
        bool showImport = false;
        bool showConfirm = false;
        bool showRenameAsset = false;
        bool exitRequested = false;
        bool focusDetails = false;
        ImGuizmo::OPERATION gizmoOperation = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;

        void rememberProject() {
            if (projectSession == nullptr || !projectSession->hasProject()) {
                return;
            }
            const auto path = projectSession->projectFile();
            std::erase(recentProjects, path);
            recentProjects.insert(recentProjects.begin(), path);
            if (recentProjects.size() > 10) {
                recentProjects.resize(10);
            }
            if (dialogs.saveRecentProjects) {
                dialogs.saveRecentProjects(recentProjects);
            }
        }

        project::ProjectRenderSettings currentProjectRenderSettings() const noexcept {
            return {.directLighting = settings.directLighting.enabled,
                    .shadows = settings.shadows.enabled,
                    .rayTracing = settings.globalIllumination.mode == render::GlobalIlluminationMode::RayTracing,
                    .ssao = settings.globalIllumination.ssaoEnabled,
                    .sharc = settings.globalIllumination.sharcEnabled,
                    .nrd = settings.globalIllumination.nrdEnabled,
                    .taa = settings.temporalAa.enabled,
                    .splitLambda = settings.shadows.splitLambda,
                    .shadowDistance = settings.shadows.maxDistance,
                    .exposure = settings.toneMapping.exposure};
        }

        void applyProjectRenderSettings() {
            if (projectSession == nullptr) {
                return;
            }
            const auto& value = projectSession->renderSettings();
            settings.directLighting.enabled = value.directLighting;
            settings.shadows.enabled = value.shadows;
            settings.globalIllumination.mode =
                value.rayTracing ? render::GlobalIlluminationMode::RayTracing : render::GlobalIlluminationMode::Legacy;
            settings.globalIllumination.ssaoEnabled = value.ssao;
            settings.globalIllumination.sharcEnabled = value.sharc;
            settings.globalIllumination.nrdEnabled = value.nrd;
            settings.temporalAa.enabled = value.taa;
            settings.shadows.splitLambda = value.splitLambda;
            settings.shadows.maxDistance = value.shadowDistance;
            settings.toneMapping.exposure = value.exposure;
        }

        void markProjectDirty() noexcept {
            if (projectSession != nullptr) {
                projectSession->markDirty();
            }
        }

        void clearSelection() noexcept {
            selection = SelectionState::Empty;
            actor.reset();
            model.reset();
            script.reset();
        }

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
            ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
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
                ImGui::DockBuilderDockWindow("Content Browser", left);
                ImGui::DockBuilderDockWindow("Details", left);
                ImGui::DockBuilderDockWindow("Render / GI", bottom);
                ImGui::DockBuilderDockWindow("Script Console", bottom);
                ImGui::DockBuilderDockWindow("Viewport", center);
            } else {
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, style::HierarchyRatio, &left, &center);
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, style::PropertiesRatio, &right, &center);
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, style::ConsoleRatio, &bottom, &center);
                ImGuiID inspector = 0;
                ImGuiID renderGi = 0;
                ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, style::InspectorRatio, &inspector, &renderGi);
                ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
                ImGui::DockBuilderDockWindow("Content Browser", left);
                ImGui::DockBuilderDockWindow("Details", inspector);
                ImGui::DockBuilderDockWindow("Render / GI", renderGi);
                ImGui::DockBuilderDockWindow("Script Console", bottom);
                ImGui::DockBuilderDockWindow("Viewport", center);
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

        void runDestructive(std::function<void()> action) {
            if (projectSession != nullptr && projectSession->dirty()) {
                pendingDestructiveAction = std::move(action);
                showConfirm = true;
                return;
            }
            action();
        }

        void requestNewProject() {
            if (!dialogs.openFolder || projectSession == nullptr) {
                statusMessage = "Folder dialog service is unavailable.";
                return;
            }
            dialogs.openFolder([this](std::vector<std::filesystem::path> paths) {
                if (paths.empty()) {
                    return;
                }
                newProjectLocation = std::move(paths.front());
                projectName.fill('\0');
                showNewProject = true;
            });
        }

        void openProjectPath(const std::filesystem::path& path) {
            if (projectSession == nullptr) {
                return;
            }
            std::string error;
            if (!projectSession->open(path, error)) {
                statusMessage = std::move(error);
                return;
            }
            clearSelection();
            applyProjectRenderSettings();
            rememberProject();
            statusMessage = "Opened " + projectSession->manifest().name;
        }

        void requestOpenProject() {
            if (!dialogs.openFiles || projectSession == nullptr) {
                statusMessage = "Project dialog service is unavailable.";
                return;
            }
            dialogs.openFiles(EditorFileDialogKind::Project, false, [this](std::vector<std::filesystem::path> paths) {
                if (!paths.empty()) {
                    openProjectPath(paths.front());
                }
            });
        }

        void saveProject() {
            if (projectSession == nullptr || !projectSession->hasProject()) {
                statusMessage = "No project is open.";
                return;
            }
            projectSession->setRenderSettings(currentProjectRenderSettings());
            std::string error;
            statusMessage = projectSession->save(error) ? "Project saved." : std::move(error);
        }

        void requestImport() {
            if (!dialogs.openFiles || projectSession == nullptr || !projectSession->hasProject()) {
                statusMessage = "Open a project before importing assets.";
                return;
            }
            dialogs.openFiles(EditorFileDialogKind::Asset, true, [this](std::vector<std::filesystem::path> paths) {
                if (!paths.empty()) {
                    importSources = std::move(paths);
                    showImport = true;
                }
            });
        }

        void drawMainMenu() {
            if (!ImGui::BeginMainMenuBar()) {
                return;
            }
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Project...")) {
                    runDestructive([this] {
                        requestNewProject();
                    });
                }
                if (ImGui::MenuItem("Open Project...")) {
                    runDestructive([this] {
                        requestOpenProject();
                    });
                }
                if (ImGui::BeginMenu("Recent Projects", !recentProjects.empty())) {
                    for (const auto& recent : recentProjects) {
                        if (ImGui::MenuItem(recent.filename().string().c_str())) {
                            runDestructive([this, recent] {
                                openProjectPath(recent);
                            });
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save Scene", "Ctrl+S", false,
                                    projectSession != nullptr && projectSession->hasProject())) {
                    saveProject();
                }
                if (ImGui::MenuItem("Import Assets...", nullptr, false,
                                    projectSession != nullptr && projectSession->hasProject())) {
                    requestImport();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) {
                    runDestructive([this] {
                        exitRequested = true;
                    });
                }
                ImGui::EndMenu();
            }
            if (projectSession != nullptr && projectSession->hasProject()) {
                ImGui::Separator();
                ImGui::TextUnformatted(projectSession->manifest().name.c_str());
                if (projectSession->dirty()) {
                    ImGui::SameLine();
                    ImGui::TextUnformatted("*");
                }
            }
            ImGui::EndMainMenuBar();
        }

        void drawProjectModals() {
            if (showNewProject) {
                ImGui::OpenPopup("New Project");
                showNewProject = false;
            }
            if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("Location: %s", newProjectLocation.generic_string().c_str());
                ImGui::SetNextItemWidth(360.0f);
                ImGui::InputTextWithHint("##projectName", "Project name", projectName.data(), projectName.size());
                const bool canCreate = projectName.front() != '\0';
                if (!canCreate) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Create")) {
                    std::string error;
                    if (projectSession->create(newProjectLocation, projectName.data(), error)) {
                        clearSelection();
                        rememberProject();
                        statusMessage = "Project created.";
                        ImGui::CloseCurrentPopup();
                    } else {
                        statusMessage = std::move(error);
                    }
                }
                if (!canCreate) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            if (showImport) {
                ImGui::OpenPopup("Import Assets");
                showImport = false;
            }
            if (ImGui::BeginPopupModal("Import Assets", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%zu file(s)", importSources.size());
                if (ImGui::BeginChild("ImportFiles", {520.0f, 180.0f}, true)) {
                    for (const auto& path : importSources) {
                        const auto type = project::assetTypeForPath(path);
                        ImGui::Text("%s  [%s]", path.filename().string().c_str(),
                                    type.has_value() ? project::assetTypeName(*type) : "Unsupported");
                    }
                }
                ImGui::EndChild();
                constexpr const char* policies[] = {"Skip", "Replace", "Auto rename"};
                int policy = static_cast<int>(importConflict);
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Conflict", &policy, policies, 3)) {
                    importConflict = static_cast<project::ImportConflictPolicy>(policy);
                }
                if (ImGui::Button("Import")) {
                    std::vector<project::ImportRequest> requests;
                    requests.reserve(importSources.size());
                    for (const auto& source : importSources) {
                        requests.push_back({.source = source, .destinationDirectory = {}, .conflict = importConflict});
                    }
                    const auto results = projectSession->importAssets(requests);
                    const auto failures = std::ranges::count_if(results, [](const auto& result) {
                        return !result.succeeded();
                    });
                    statusMessage = std::to_string(results.size() - failures) + " imported, " +
                                    std::to_string(failures) + " failed.";
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            if (showConfirm) {
                ImGui::OpenPopup("Unsaved Changes");
                showConfirm = false;
            }
            if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Save the current project before continuing?");
                if (ImGui::Button("Save")) {
                    saveProject();
                    if (projectSession == nullptr || !projectSession->dirty()) {
                        auto action = std::move(pendingDestructiveAction);
                        ImGui::CloseCurrentPopup();
                        if (action) {
                            action();
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard")) {
                    auto action = std::move(pendingDestructiveAction);
                    ImGui::CloseCurrentPopup();
                    if (action) {
                        action();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    pendingDestructiveAction = {};
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        void drawContentBrowser() {
            ImGui::Begin("Content Browser");
            if (projectSession == nullptr || !projectSession->hasProject()) {
                ImGui::TextDisabled("Open or create a project to browse content.");
                ImGui::End();
                return;
            }
            if (ImGui::Button("Import")) {
                requestImport();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##contentSearch", "Search assets", contentSearch.data(), contentSearch.size());
            const std::string_view query{contentSearch.data()};
            std::optional<project::AssetId> removeAsset;
            if (ImGui::BeginTable("Assets", 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableHeadersRow();
                for (const project::AssetRecord& asset : projectSession->assets().assets()) {
                    if (!query.empty() && asset.displayName.find(query) == std::string::npos) {
                        continue;
                    }
                    ImGui::PushID(asset.id.value.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(asset.displayName.c_str(), false,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && asset.type == project::AssetType::Mesh) {
                        if (const auto created = projectSession->createActorFromMesh(asset.id); created.has_value()) {
                            selectActor(*created);
                        }
                    }
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("LUMIN_ASSET", asset.id.value.data(), asset.id.value.size());
                        ImGui::TextUnformatted(asset.displayName.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginPopupContextItem("AssetContext")) {
                        if (ImGui::MenuItem("Rename")) {
                            renameAssetId = asset.id;
                            renameAssetName.fill('\0');
                            const std::size_t length = std::min(asset.displayName.size(), renameAssetName.size() - 1);
                            std::copy_n(asset.displayName.data(), length, renameAssetName.data());
                            showRenameAsset = true;
                        }
                        if (ImGui::MenuItem("Delete")) {
                            removeAsset = asset.id;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(project::assetTypeName(asset.type));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (removeAsset.has_value()) {
                std::string error;
                if (!projectSession->removeAsset(*removeAsset, error)) {
                    statusMessage = std::move(error);
                }
            }
            if (showRenameAsset) {
                ImGui::OpenPopup("Rename Asset");
                showRenameAsset = false;
            }
            if (renameAssetId.has_value() &&
                ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("Name", renameAssetName.data(), renameAssetName.size());
                if (ImGui::Button("Rename")) {
                    std::string error;
                    if (projectSession->renameAsset(*renameAssetId, renameAssetName.data(), error)) {
                        renameAssetId.reset();
                        ImGui::CloseCurrentPopup();
                    } else {
                        statusMessage = std::move(error);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    renameAssetId.reset();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (!statusMessage.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", statusMessage.c_str());
            }
            ImGui::End();
        }

        void drawHierarchy() {
            ImGui::Begin("Scene Hierarchy");
            for (const scene::ActorHandle handle : level.actorHandles()) {
                scene::Actor* value = level.actor(handle);
                if (value == nullptr) {
                    continue;
                }
                const std::string label =
                    value->name() + "##actor" + std::to_string(handle.index) + ":" + std::to_string(handle.generation);
                const bool selected = actor == handle;
                ImGui::PushID(static_cast<int>(handle.index));
                ImGui::PushID(static_cast<int>(handle.generation));
                if (ImGui::Selectable(label.c_str(), selected)) {
                    selectActor(handle);
                }
                if (ImGui::BeginPopupContextItem("ActorContext")) {
                    selectActor(handle);
                    if (ImGui::MenuItem("Properties")) {
                        focusDetails = true;
                    }
                    if (projectSession != nullptr && projectSession->hasProject() && ImGui::BeginMenu("Add Script")) {
                        for (const project::AssetRecord& asset : projectSession->assets().assets()) {
                            if (asset.type == project::AssetType::Script &&
                                ImGui::MenuItem(asset.displayName.c_str())) {
                                const auto result =
                                    scripts.attach(level, handle, projectSession->rootDirectory() / asset.relativePath);
                                if (!result && result.result.error.has_value()) {
                                    statusMessage = result.result.error->message;
                                } else {
                                    markProjectDirty();
                                }
                            }
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Duplicate") && projectSession != nullptr && value->modelHandle().isValid()) {
                        const auto meshAsset = projectSession->assetForMesh(level.model(value->modelHandle()).mesh);
                        if (meshAsset.has_value()) {
                            scene::Transform transform = value->transform();
                            transform.position.x += 0.5f;
                            if (const auto copy = projectSession->createActorFromMesh(*meshAsset, transform);
                                copy.has_value()) {
                                level.actor(*copy)->setMaterial(value->material());
                                selectActor(*copy);
                            }
                        }
                    }
                    if (ImGui::MenuItem("Delete")) {
                        level.destroyActor(handle);
                        clearSelection();
                        markProjectDirty();
                    }
                    ImGui::EndPopup();
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
                markProjectDirty();
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
            if (projectSession != nullptr && projectSession->hasProject()) {
                if (!material.textures.has_value()) {
                    material.textures = scene::PbrTextureSet{};
                }
                const auto textureSlot = [&](const char* label, const char* id, std::filesystem::path& target) {
                    propertyLabel(label);
                    const std::string value = target.empty() ? "None" : target.filename().string();
                    ImGui::Button((value + id).c_str(), {-1.0f, 0.0f});
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LUMIN_ASSET");
                            payload != nullptr) {
                            const project::AssetId asset{std::string{static_cast<const char*>(payload->Data),
                                                                     static_cast<std::size_t>(payload->DataSize)}};
                            if (const project::AssetRecord* record = projectSession->assets().find(asset);
                                record != nullptr && record->type == project::AssetType::Texture) {
                                target = projectSession->rootDirectory() / record->relativePath;
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                };
                textureSlot("Base color", "##baseColorTexture", material.textures->baseColor);
                textureSlot("Normal", "##normalTexture", material.textures->normal);
                textureSlot("Roughness", "##roughnessTexture", material.textures->roughness);
            }
            if (changed) {
                setSelectedMaterial(material);
                markProjectDirty();
            }
        }

        void drawInspector() {
            if (focusDetails) {
                ImGui::SetNextWindowFocus();
                focusDetails = false;
            }
            ImGui::Begin("Details");
            if (selection == SelectionState::Stale) {
                ImGui::TextUnformatted("Selection became stale and was cleared.");
            } else if (actor.has_value()) {
                scene::Actor* selected = level.actor(*actor);
                if (selected != nullptr) {
                    std::array<char, 128> name{};
                    const std::size_t nameLength = std::min(selected->name().size(), name.size() - 1);
                    std::copy_n(selected->name().data(), nameLength, name.data());
                    propertyLabel("Name");
                    if (ImGui::InputText("##actorName", name.data(), name.size(),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        selected->setName(name.data());
                        markProjectDirty();
                    }
                    section("Transform");
                    editTransform(selected->transform());
                    section("Material");
                    editMaterial(selected->material());
                    section("Scripts");
                    std::vector<scripting::ScriptInfo> attached = scripts.scriptsForActor(*actor);
                    for (std::size_t index = 0; index < attached.size(); ++index) {
                        scripting::ScriptInfo& info = attached[index];
                        ImGui::PushID(static_cast<int>(info.handle.value));
                        bool enabled = info.enabled;
                        if (ImGui::Checkbox("##enabled", &enabled)) {
                            scripts.setEnabled(info.handle, enabled);
                            markProjectDirty();
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(info.source.filename().string().c_str());
                        if (ImGui::SmallButton("Reload")) {
                            static_cast<void>(scripts.reload(info.handle));
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Up") && index > 0) {
                            scripts.reorder(info.handle, index - 1);
                            markProjectDirty();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Down") && index + 1 < attached.size()) {
                            scripts.reorder(info.handle, index + 1);
                            markProjectDirty();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            scripts.detach(info.handle);
                            markProjectDirty();
                        }
                        ImGui::PopID();
                    }
                    if (projectSession != nullptr && projectSession->hasProject() &&
                        ImGui::BeginCombo("Add Script", "Select")) {
                        for (const project::AssetRecord& asset : projectSession->assets().assets()) {
                            if (asset.type == project::AssetType::Script &&
                                ImGui::Selectable(asset.displayName.c_str())) {
                                const auto result =
                                    scripts.attach(level, *actor, projectSession->rootDirectory() / asset.relativePath);
                                if (!result && result.result.error.has_value()) {
                                    statusMessage = result.result.error->message;
                                } else {
                                    markProjectDirty();
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
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

        void drawViewport() {
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
            ImGui::Begin("Viewport", nullptr, flags);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
            if (ImGui::RadioButton("Move", gizmoOperation == ImGuizmo::TRANSLATE)) {
                gizmoOperation = ImGuizmo::TRANSLATE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate", gizmoOperation == ImGuizmo::ROTATE)) {
                gizmoOperation = ImGuizmo::ROTATE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale", gizmoOperation == ImGuizmo::SCALE)) {
                gizmoOperation = ImGuizmo::SCALE;
            }
            ImGui::SameLine();
            if (ImGui::Button(gizmoMode == ImGuizmo::WORLD ? "World" : "Local")) {
                gizmoMode = gizmoMode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
            }
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
            viewportInteraction.width =
                available.x > 0.0f ? static_cast<std::uint32_t>(std::max(available.x * framebufferScale.x, 1.0f)) : 0U;
            viewportInteraction.height =
                available.y > 0.0f ? static_cast<std::uint32_t>(std::max(available.y * framebufferScale.y, 1.0f)) : 0U;
            viewportInteraction.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

            const render::ImGuiViewportImage image = viewportImage ? viewportImage() : render::ImGuiViewportImage{};
            if (image.isValid() && available.x > 0.0f && available.y > 0.0f) {
                ImGui::Image(static_cast<ImTextureID>(image.textureId), available);
            } else {
                ImGui::Dummy(available);
            }
            viewportInteraction.hovered = ImGui::IsItemHovered();
            const ImVec2 imageMinimum = ImGui::GetItemRectMin();
            const ImVec2 imageMaximum = ImGui::GetItemRectMax();
            const auto pickAtMouse = [&]() -> std::optional<ViewportPickResult> {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const ViewportRay ray =
                    makeViewportRay(camera, mouse.x - imageMinimum.x, mouse.y - imageMinimum.y,
                                    imageMaximum.x - imageMinimum.x, imageMaximum.y - imageMinimum.y);
                return pickViewportModel(level, ray);
            };
            const auto selectPick = [&](const std::optional<ViewportPickResult>& picked) {
                if (!picked.has_value()) {
                    clearSelection();
                } else if (const auto owner = level.actorForModel(picked->model); owner.has_value()) {
                    selectActor(*owner);
                } else {
                    selectModel(picked->model);
                }
            };

            scene::Transform selectedTransform;
            bool hasSelectedTransform = false;
            if (actor.has_value()) {
                if (const scene::Actor* selected = level.actor(*actor); selected != nullptr) {
                    selectedTransform = selected->transform();
                    hasSelectedTransform = true;
                }
            } else if (model.has_value() && modelAlive(level, *model)) {
                selectedTransform = level.model(*model).transform;
                hasSelectedTransform = true;
            }
            if (hasSelectedTransform) {
                glm::mat4 transformMatrix = selectedTransform.matrix();
                const glm::mat4 view = camera.viewMatrix();
                const glm::mat4 projection = camera.projectionMatrix(std::max(imageMaximum.x - imageMinimum.x, 1.0f) /
                                                                     std::max(imageMaximum.y - imageMinimum.y, 1.0f));
                ImGuizmo::SetOrthographic(false);
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(imageMinimum.x, imageMinimum.y, imageMaximum.x - imageMinimum.x,
                                  imageMaximum.y - imageMinimum.y);
                if (ImGuizmo::Manipulate(&view[0][0], &projection[0][0], gizmoOperation, gizmoMode,
                                         &transformMatrix[0][0])) {
                    ImGuizmo::DecomposeMatrixToComponents(&transformMatrix[0][0], &selectedTransform.position.x,
                                                          &selectedTransform.rotationDegrees.x,
                                                          &selectedTransform.scale.x);
                    setSelectedTransform(selectedTransform);
                    markProjectDirty();
                }
            }

            if (viewportInteraction.hovered && viewportInteraction.focused && !ImGui::GetIO().WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_W)) {
                    gizmoOperation = ImGuizmo::TRANSLATE;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_E)) {
                    gizmoOperation = ImGuizmo::ROTATE;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                    gizmoOperation = ImGuizmo::SCALE;
                }
            }
            if (viewportInteraction.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() &&
                !ImGuizmo::IsUsing()) {
                selectPick(pickAtMouse());
            }
            if (viewportInteraction.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGuizmo::IsUsing()) {
                selectPick(pickAtMouse());
                if (actor.has_value() || model.has_value()) {
                    ImGui::OpenPopup("ViewportObjectContext");
                }
            }
            if (ImGui::BeginPopup("ViewportObjectContext")) {
                if (ImGui::MenuItem("Properties")) {
                    focusDetails = true;
                }
                if (actor.has_value() && ImGui::MenuItem("Delete")) {
                    level.destroyActor(*actor);
                    clearSelection();
                    markProjectDirty();
                }
                ImGui::EndPopup();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LUMIN_ASSET");
                    payload != nullptr && projectSession != nullptr) {
                    const project::AssetId asset{std::string{static_cast<const char*>(payload->Data),
                                                             static_cast<std::size_t>(payload->DataSize)}};
                    scene::Transform transform;
                    const auto picked = pickAtMouse();
                    transform.position =
                        picked.has_value() ? picked->worldPosition : camera.position() + camera.forward() * 3.0f;
                    if (const auto created = projectSession->createActorFromMesh(asset, transform);
                        created.has_value()) {
                        selectActor(*created);
                    }
                }
                ImGui::EndDragDropTarget();
            }
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
            section("Global Illumination");
            const char* modeName =
                settings.globalIllumination.mode == render::GlobalIlluminationMode::Legacy ? "Legacy" : "Ray Tracing";
            propertyLabel("Mode");
            ImGui::SetNextItemWidth(-style::Space1);
            if (ImGui::BeginCombo("##renderMode", modeName)) {
                for (const auto [mode, name] : {std::pair{render::GlobalIlluminationMode::Legacy, "Legacy"},
                                                std::pair{render::GlobalIlluminationMode::RayTracing, "Ray Tracing"}}) {
                    const bool selected = settings.globalIllumination.mode == mode;
                    if (ImGui::Selectable(name, selected)) {
                        settings.globalIllumination.mode = mode;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (settings.globalIllumination.mode == render::GlobalIlluminationMode::Legacy) {
                ImGui::Checkbox("SSAO", &settings.globalIllumination.ssaoEnabled);
                ImGui::Checkbox("CSM", &settings.shadows.enabled);
                if (settings.shadows.enabled) {
                    propertyLabel("Split lambda");
                    ImGui::SliderFloat("##csmSplitLambda", &settings.shadows.splitLambda, 0.0f, 1.0f, "%.2f");
                    propertyLabel("Max distance");
                    ImGui::SliderFloat("##csmMaxDistance", &settings.shadows.maxDistance, 1.0f,
                                       std::max(camera.farPlane(), 1.0f), "%.1f");
                }
            } else {
                ImGui::Checkbox("SHARC", &settings.globalIllumination.sharcEnabled);
                ImGui::Checkbox("NRD", &settings.globalIllumination.nrdEnabled);
            }
            const render::gi::BackendInfo backend = backendInfo();
            ImGui::Text("Active: %.*s", static_cast<int>(backend.name.size()), backend.name.data());
            section("Temporal AA");
            ImGui::Checkbox("TAA", &settings.temporalAa.enabled);
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
                   scripting::ScriptRuntime& scripts, BackendInfoProvider backendInfo,
                   ViewportImageProvider viewportImage, project::ProjectSession* project, EditorDialogServices dialogs)
        : impl_(std::make_unique<Impl>(level, camera, settings, scripts, std::move(backendInfo),
                                       std::move(viewportImage), project, std::move(dialogs))) {
    }

    Editor::~Editor() = default;
    Editor::Editor(Editor&&) noexcept = default;
    Editor& Editor::operator=(Editor&&) noexcept = default;

    void Editor::draw() {
        style::apply();
        synchronizeSelection();
        ImGuizmo::BeginFrame();
        impl_->drawMainMenu();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImGuiID dockspace = ImGui::GetID("LuminEditorDockspace");
        impl_->buildLayout(dockspace, *viewport);
        ImGui::DockSpaceOverViewport(dockspace, viewport);
        impl_->drawHierarchy();
        impl_->drawContentBrowser();
        impl_->drawInspector();
        impl_->drawViewport();
        impl_->drawRenderSettings();
        impl_->drawConsole();
        impl_->drawProjectModals();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            impl_->saveProject();
        }
    }

    ViewportInteractionState Editor::viewportInteraction() const noexcept {
        return impl_->viewportInteraction;
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
        impl_->clearSelection();
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

    void Editor::setGlobalIlluminationMode(render::GlobalIlluminationMode mode) noexcept {
        impl_->settings.globalIllumination.mode = mode;
    }

    void Editor::setSsaoEnabled(bool enabled) noexcept {
        impl_->settings.globalIllumination.ssaoEnabled = enabled;
    }

    void Editor::setSharcEnabled(bool enabled) noexcept {
        impl_->settings.globalIllumination.sharcEnabled = enabled;
    }

    void Editor::setNrdEnabled(bool enabled) noexcept {
        impl_->settings.globalIllumination.nrdEnabled = enabled;
    }

    void Editor::setCsmSplitLambda(float splitLambda) noexcept {
        impl_->settings.shadows.splitLambda = std::clamp(splitLambda, 0.0f, 1.0f);
    }

    void Editor::setCsmMaxDistance(float maxDistance) noexcept {
        impl_->settings.shadows.maxDistance = std::max(maxDistance, 1.0f);
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

    bool Editor::exitRequested() const noexcept {
        return impl_->exitRequested;
    }

    void Editor::requestExit() {
        impl_->runDestructive([this] {
            impl_->exitRequested = true;
        });
    }

} // namespace lumin::editor

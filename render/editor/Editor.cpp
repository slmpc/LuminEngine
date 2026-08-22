#include "render/editor/Editor.hpp"

#include "EditorStyle.hpp"
#include "render/editor/EditorLayout.hpp"
#include "render/editor/ViewportPicking.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>

// clang-format off
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
// clang-format on

namespace lumin::editor {
    namespace {

        project::ProjectAmbientOcclusionMode toProjectAmbientOcclusionMode(render::AmbientOcclusionMode mode) noexcept {
            switch (mode) {
            case render::AmbientOcclusionMode::Hbao:
                return project::ProjectAmbientOcclusionMode::Hbao;
            case render::AmbientOcclusionMode::Gtao:
                return project::ProjectAmbientOcclusionMode::Gtao;
            case render::AmbientOcclusionMode::Ssao:
            default:
                return project::ProjectAmbientOcclusionMode::Ssao;
            }
        }

        render::AmbientOcclusionMode toRenderAmbientOcclusionMode(project::ProjectAmbientOcclusionMode mode) noexcept {
            switch (mode) {
            case project::ProjectAmbientOcclusionMode::Hbao:
                return render::AmbientOcclusionMode::Hbao;
            case project::ProjectAmbientOcclusionMode::Gtao:
                return render::AmbientOcclusionMode::Gtao;
            case project::ProjectAmbientOcclusionMode::Ssao:
            default:
                return render::AmbientOcclusionMode::Ssao;
            }
        }

        const char* ambientOcclusionModeName(render::AmbientOcclusionMode mode) noexcept {
            switch (mode) {
            case render::AmbientOcclusionMode::Hbao:
                return "HBAO";
            case render::AmbientOcclusionMode::Gtao:
                return "GTAO";
            case render::AmbientOcclusionMode::Ssao:
            default:
                return "SSAO";
            }
        }

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

        enum class BrowserIconKind {
            Folder,
            Mesh,
            Texture,
            Script,
            Scene,
            Project,
            File,
        };

        enum class ToolbarIcon {
            Back,
            Up,
            Refresh,
        };

        std::string lowerAscii(std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        bool pathWithin(const std::filesystem::path& path, const std::filesystem::path& directory) {
            if (directory.empty()) {
                return true;
            }
            const auto relative = path.lexically_relative(directory);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
        }

        BrowserIconKind browserIconFor(const project::ProjectEntry& entry, const project::AssetRecord* asset) {
            if (entry.kind == project::ProjectEntryKind::Directory) {
                return BrowserIconKind::Folder;
            }
            if (asset != nullptr) {
                switch (asset->type) {
                case project::AssetType::Mesh:
                    return BrowserIconKind::Mesh;
                case project::AssetType::Texture:
                    return BrowserIconKind::Texture;
                case project::AssetType::Script:
                    return BrowserIconKind::Script;
                }
            }
            if (entry.relativePath.extension() == ".scene") {
                return BrowserIconKind::Scene;
            }
            if (entry.relativePath.extension() == ".luminproject" ||
                entry.relativePath.filename() == "AssetRegistry.json") {
                return BrowserIconKind::Project;
            }
            return BrowserIconKind::File;
        }

        ImU32 iconColor(BrowserIconKind kind) {
            switch (kind) {
            case BrowserIconKind::Folder:
                return IM_COL32(222, 177, 82, 255);
            case BrowserIconKind::Mesh:
                return IM_COL32(103, 174, 200, 255);
            case BrowserIconKind::Texture:
                return IM_COL32(109, 190, 132, 255);
            case BrowserIconKind::Script:
                return IM_COL32(224, 137, 105, 255);
            case BrowserIconKind::Scene:
                return IM_COL32(185, 135, 207, 255);
            case BrowserIconKind::Project:
                return IM_COL32(211, 197, 151, 255);
            case BrowserIconKind::File:
                return IM_COL32(155, 161, 168, 255);
            }
            return IM_COL32_WHITE;
        }

        void drawBrowserIcon(ImDrawList& drawList, ImVec2 origin, float size, BrowserIconKind kind) {
            const ImU32 color = iconColor(kind);
            const ImU32 muted = IM_COL32(68, 73, 79, 255);
            const float stroke = std::max(1.5f, size * 0.055f);
            const ImVec2 minimum{origin.x + size * 0.12f, origin.y + size * 0.18f};
            const ImVec2 maximum{origin.x + size * 0.88f, origin.y + size * 0.82f};
            if (kind == BrowserIconKind::Folder) {
                drawList.AddRectFilled({minimum.x, origin.y + size * 0.30f}, maximum, color, size * 0.07f);
                drawList.AddRectFilled(minimum, {origin.x + size * 0.48f, origin.y + size * 0.42f}, color,
                                       size * 0.06f);
                drawList.AddLine({minimum.x, origin.y + size * 0.45f}, {maximum.x, origin.y + size * 0.45f},
                                 IM_COL32(255, 255, 255, 65), stroke);
                return;
            }
            if (kind == BrowserIconKind::Mesh) {
                const ImVec2 top{origin.x + size * 0.50f, origin.y + size * 0.13f};
                const ImVec2 left{origin.x + size * 0.17f, origin.y + size * 0.33f};
                const ImVec2 right{origin.x + size * 0.83f, origin.y + size * 0.33f};
                const ImVec2 bottomLeft{left.x, origin.y + size * 0.70f};
                const ImVec2 bottomRight{right.x, bottomLeft.y};
                const ImVec2 bottom{top.x, origin.y + size * 0.88f};
                drawList.AddLine(top, left, color, stroke);
                drawList.AddLine(top, right, color, stroke);
                drawList.AddLine(left, right, color, stroke);
                drawList.AddLine(left, bottomLeft, color, stroke);
                drawList.AddLine(right, bottomRight, color, stroke);
                drawList.AddLine(bottomLeft, bottom, color, stroke);
                drawList.AddLine(bottomRight, bottom, color, stroke);
                drawList.AddLine(top, bottom, color, stroke);
                return;
            }
            drawList.AddRectFilled(minimum, maximum, muted, size * 0.045f);
            drawList.AddRect(minimum, maximum, color, size * 0.045f, 0, stroke);
            if (kind == BrowserIconKind::Texture) {
                drawList.AddCircleFilled({origin.x + size * 0.68f, origin.y + size * 0.36f}, size * 0.075f, color);
                const ImVec2 points[] = {{origin.x + size * 0.20f, origin.y + size * 0.72f},
                                         {origin.x + size * 0.40f, origin.y + size * 0.47f},
                                         {origin.x + size * 0.54f, origin.y + size * 0.62f},
                                         {origin.x + size * 0.66f, origin.y + size * 0.53f},
                                         {origin.x + size * 0.81f, origin.y + size * 0.72f}};
                drawList.AddPolyline(points, static_cast<int>(std::size(points)), color, 0, stroke);
            } else if (kind == BrowserIconKind::Script) {
                drawList.AddPolyline(std::array<ImVec2, 3>{{{origin.x + size * 0.42f, origin.y + size * 0.36f},
                                                            {origin.x + size * 0.30f, origin.y + size * 0.50f},
                                                            {origin.x + size * 0.42f, origin.y + size * 0.64f}}}
                                         .data(),
                                     3, color, 0, stroke);
                drawList.AddPolyline(std::array<ImVec2, 3>{{{origin.x + size * 0.58f, origin.y + size * 0.36f},
                                                            {origin.x + size * 0.70f, origin.y + size * 0.50f},
                                                            {origin.x + size * 0.58f, origin.y + size * 0.64f}}}
                                         .data(),
                                     3, color, 0, stroke);
            } else if (kind == BrowserIconKind::Scene) {
                const ImVec2 center{origin.x + size * 0.50f, origin.y + size * 0.56f};
                drawList.AddCircle(center, size * 0.17f, color, 0, stroke);
                drawList.AddLine(center, {center.x, origin.y + size * 0.27f}, IM_COL32(109, 190, 132, 255), stroke);
                drawList.AddLine(center, {origin.x + size * 0.73f, origin.y + size * 0.69f},
                                 IM_COL32(224, 137, 105, 255), stroke);
                drawList.AddLine(center, {origin.x + size * 0.27f, origin.y + size * 0.69f},
                                 IM_COL32(103, 174, 200, 255), stroke);
            } else {
                for (int line = 0; line < 3; ++line) {
                    const float y = origin.y + size * (0.38f + 0.14f * static_cast<float>(line));
                    drawList.AddLine({origin.x + size * 0.28f, y}, {origin.x + size * 0.72f, y}, color, stroke);
                }
            }
        }

        bool toolbarIconButton(const char* id, ToolbarIcon icon, bool enabled = true) {
            const float side = ImGui::GetFrameHeight();
            if (!enabled) {
                ImGui::BeginDisabled();
            }
            const bool pressed = ImGui::InvisibleButton(id, {side, side});
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 center{minimum.x + side * 0.5f, minimum.y + side * 0.5f};
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            const float stroke = 1.8f;
            if (icon == ToolbarIcon::Back) {
                drawList->AddLine({center.x + side * 0.22f, center.y}, {center.x - side * 0.18f, center.y}, color,
                                  stroke);
                drawList->AddPolyline(std::array<ImVec2, 3>{{{center.x - side * 0.04f, center.y - side * 0.17f},
                                                             {center.x - side * 0.20f, center.y},
                                                             {center.x - side * 0.04f, center.y + side * 0.17f}}}
                                          .data(),
                                      3, color, 0, stroke);
            } else if (icon == ToolbarIcon::Up) {
                drawList->AddLine({center.x, center.y + side * 0.22f}, {center.x, center.y - side * 0.18f}, color,
                                  stroke);
                drawList->AddPolyline(std::array<ImVec2, 3>{{{center.x - side * 0.17f, center.y - side * 0.03f},
                                                             {center.x, center.y - side * 0.20f},
                                                             {center.x + side * 0.17f, center.y - side * 0.03f}}}
                                          .data(),
                                      3, color, 0, stroke);
            } else {
                drawList->AddCircle(center, side * 0.22f, color, 0, stroke);
                const ImVec2 triangle[] = {{center.x + side * 0.12f, center.y - side * 0.23f},
                                           {center.x + side * 0.27f, center.y - side * 0.21f},
                                           {center.x + side * 0.22f, center.y - side * 0.07f}};
                drawList->AddTriangleFilled(triangle[0], triangle[1], triangle[2], color);
            }
            if (!enabled) {
                ImGui::EndDisabled();
            }
            return enabled && pressed;
        }

    } // namespace

    struct Editor::Impl {
        Impl(scene::Level& levelValue, scene::Camera& cameraValue, render::RenderSettings& settingsValue,
             scripting::ScriptRuntime& scriptsValue, BackendInfoProvider backendInfoValue,
             ViewportImageProvider viewportImageValue, project::ProjectSession* projectValue,
             EditorDialogServices dialogsValue, EditorSettingsServices settingsServicesValue)
            : level(levelValue), camera(cameraValue), settings(settingsValue), scripts(scriptsValue),
              backendInfo(std::move(backendInfoValue)), viewportImage(std::move(viewportImageValue)),
              projectSession(projectValue), dialogs(std::move(dialogsValue)),
              engineSettings(std::move(settingsServicesValue.settings)),
              saveEngineSettings(std::move(settingsServicesValue.save)) {
            config::normalizeEngineSettings(engineSettings);
        }

        scene::Level& level;
        scene::Camera& camera;
        render::RenderSettings& settings;
        scripting::ScriptRuntime& scripts;
        BackendInfoProvider backendInfo;
        ViewportImageProvider viewportImage;
        project::ProjectSession* projectSession = nullptr;
        EditorDialogServices dialogs;
        config::EngineSettings engineSettings;
        SaveEngineSettingsCallback saveEngineSettings;
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
        std::filesystem::path contentDirectory;
        std::filesystem::path contentProjectRoot;
        std::vector<std::filesystem::path> contentHistory;
        std::chrono::steady_clock::time_point lastContentSync{};
        std::filesystem::path newProjectLocation;
        std::optional<std::filesystem::path> selectedRecentProject;
        std::string statusMessage;
        std::function<void()> pendingDestructiveAction;
        bool showNewProject = false;
        bool showConfirm = false;
        bool showRenameAsset = false;
        bool showConfiguration = false;
        bool exitRequested = false;
        bool focusDetails = false;
        ImGuizmo::OPERATION gizmoOperation = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;

        void persistEngineSettings() {
            if (!saveEngineSettings) {
                return;
            }
            std::string error;
            if (!saveEngineSettings(engineSettings, error)) {
                statusMessage = std::move(error);
            }
        }

        void rememberProject() {
            if (projectSession == nullptr || !projectSession->hasProject()) {
                return;
            }
            config::rememberProject(engineSettings, projectSession->projectFile());
            selectedRecentProject = projectSession->projectFile();
            persistEngineSettings();
        }

        project::ProjectRenderSettings currentProjectRenderSettings() const noexcept {
            return {.directLighting = settings.directLighting.enabled,
                    .shadows = settings.shadows.enabled,
                    .rayTracing = settings.globalIllumination.mode == render::GlobalIlluminationMode::RayTracing,
                    .ssao = settings.globalIllumination.ssaoEnabled,
                    .ambientOcclusionMode =
                        toProjectAmbientOcclusionMode(settings.globalIllumination.ambientOcclusionMode),
                    .ambientOcclusionRadius = settings.globalIllumination.ambientOcclusionRadius,
                    .ambientOcclusionStrength = settings.globalIllumination.ambientOcclusionStrength,
                    .ambientOcclusionBias = settings.globalIllumination.ambientOcclusionBias,
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
            settings.globalIllumination.ambientOcclusionMode = toRenderAmbientOcclusionMode(value.ambientOcclusionMode);
            settings.globalIllumination.ambientOcclusionRadius = std::max(value.ambientOcclusionRadius, 0.05f);
            settings.globalIllumination.ambientOcclusionStrength = std::max(value.ambientOcclusionStrength, 0.0f);
            settings.globalIllumination.ambientOcclusionBias = std::clamp(value.ambientOcclusionBias, 0.0f, 0.5f);
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
            ImGuiID right = 0;
            ImGuiID bottom = 0;
            ImGuiID hierarchy = 0;
            ImGuiID properties = 0;
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, style::RightPanelRatio, &right, &center);
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, style::BottomPanelRatio, &bottom, &center);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, style::SceneHierarchyRatio, &hierarchy, &properties);
            ImGui::DockBuilderDockWindow("Scene Hierarchy", hierarchy);
            ImGui::DockBuilderDockWindow("Details", properties);
            ImGui::DockBuilderDockWindow("Render / GI", properties);
            ImGui::DockBuilderDockWindow("Content Browser", bottom);
            ImGui::DockBuilderDockWindow("Script Console", bottom);
            ImGui::DockBuilderDockWindow("Viewport", center);
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

        bool openProjectPath(const std::filesystem::path& path) {
            if (projectSession == nullptr) {
                return false;
            }
            const std::filesystem::path previousProject = projectSession->projectFile();
            std::string error;
            if (!projectSession->open(path, error)) {
                if (projectSession->projectFile() != previousProject) {
                    projectSession->close();
                }
                if (engineSettings.lastProject.has_value() && engineSettings.lastProject->lexically_normal() ==
                                                                  std::filesystem::absolute(path).lexically_normal()) {
                    engineSettings.lastProject.reset();
                    if (!std::filesystem::exists(path)) {
                        std::erase(engineSettings.recentProjects, std::filesystem::absolute(path).lexically_normal());
                    }
                    persistEngineSettings();
                }
                statusMessage = std::move(error);
                return false;
            }
            clearSelection();
            applyProjectRenderSettings();
            rememberProject();
            statusMessage = "Opened " + projectSession->manifest().name;
            return true;
        }

        void requestOpenProject() {
            if (!dialogs.openProject || projectSession == nullptr) {
                statusMessage = "Project dialog service is unavailable.";
                return;
            }
            dialogs.openProject([this](std::vector<std::filesystem::path> paths) {
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

        void closeProject() {
            if (projectSession == nullptr) {
                return;
            }
            projectSession->close();
            clearSelection();
            contentProjectRoot.clear();
            contentDirectory.clear();
            contentHistory.clear();
            viewportInteraction = {};
            statusMessage = "Project closed.";
        }

        void drawMainMenu() {
            std::optional<std::filesystem::path> recentProjectToOpen;
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
                if (ImGui::BeginMenu("Recent Projects", !engineSettings.recentProjects.empty())) {
                    for (const auto& recent : engineSettings.recentProjects) {
                        ImGui::PushID(recent.generic_string().c_str());
                        if (ImGui::MenuItem(recent.filename().string().c_str())) {
                            recentProjectToOpen = recent;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save Scene", "Ctrl+S", false,
                                    projectSession != nullptr && projectSession->hasProject())) {
                    saveProject();
                }
                if (ImGui::MenuItem("Close Project", nullptr, false,
                                    projectSession != nullptr && projectSession->hasProject())) {
                    runDestructive([this] {
                        closeProject();
                    });
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Configuration")) {
                    showConfiguration = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) {
                    runDestructive([this] {
                        exitRequested = true;
                    });
                }
                ImGui::EndMenu();
            }
            const bool editorWorkspaceActive = projectSession == nullptr || projectSession->hasProject();
            if (ImGui::BeginMenu("View", editorWorkspaceActive)) {
                auto& windows = engineSettings.windows;
                ImGui::MenuItem("Viewport", nullptr, &windows.viewport);
                ImGui::MenuItem("Scene Hierarchy", nullptr, &windows.sceneHierarchy);
                ImGui::MenuItem("Details", nullptr, &windows.details);
                ImGui::MenuItem("Content Browser", nullptr, &windows.contentBrowser);
                ImGui::MenuItem("Script Console", nullptr, &windows.scriptConsole);
                ImGui::MenuItem("Render / GI", nullptr, &windows.renderSettings);
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

            if (recentProjectToOpen.has_value()) {
                runDestructive([this, recent = std::move(*recentProjectToOpen)] {
                    openProjectPath(recent);
                });
            }
        }

        void drawProjectNavigator() {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
            if (!ImGui::Begin("Project Navigator", nullptr, flags)) {
                ImGui::End();
                return;
            }

            ImGui::TextUnformatted("Lumin Engine");
            ImGui::SeparatorText("Projects");
            if (ImGui::Button("New Project...")) {
                requestNewProject();
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Project...")) {
                requestOpenProject();
            }
            ImGui::SameLine();
            const bool canOpenSelected = selectedRecentProject.has_value();
            if (!canOpenSelected) {
                ImGui::BeginDisabled();
            }
            const bool openSelected = ImGui::Button("Open Selected");
            if (!canOpenSelected) {
                ImGui::EndDisabled();
            }

            std::optional<std::filesystem::path> projectToOpen;
            if (openSelected && selectedRecentProject.has_value()) {
                projectToOpen = selectedRecentProject;
            }
            ImGui::SeparatorText("Recent Projects");
            if (engineSettings.recentProjects.empty()) {
                ImGui::TextDisabled("No recent projects");
            } else if (ImGui::BeginTable("RecentProjectTable", 2,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                             ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableHeadersRow();
                for (const auto& recent : engineSettings.recentProjects) {
                    ImGui::PushID(recent.generic_string().c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool selected = selectedRecentProject.has_value() && *selectedRecentProject == recent;
                    const std::string name = recent.stem().string();
                    if (ImGui::Selectable(name.c_str(), selected,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedRecentProject = recent;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            projectToOpen = recent;
                        }
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(recent.generic_string().c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (!statusMessage.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", statusMessage.c_str());
            }
            ImGui::End();

            if (projectToOpen.has_value()) {
                openProjectPath(*projectToOpen);
            }
        }

        void drawConfiguration() {
            if (!showConfiguration) {
                return;
            }
            if (!ImGui::Begin("Configuration", &showConfiguration, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::End();
                return;
            }
            ImGui::SeparatorText("Startup");
            bool changed = false;
            if (ImGui::RadioButton("Project Navigator",
                                   engineSettings.startupDestination == config::StartupDestination::ProjectNavigator)) {
                engineSettings.startupDestination = config::StartupDestination::ProjectNavigator;
                changed = true;
            }
            if (ImGui::RadioButton("Last Project",
                                   engineSettings.startupDestination == config::StartupDestination::LastProject)) {
                engineSettings.startupDestination = config::StartupDestination::LastProject;
                changed = true;
            }
            ImGui::End();
            if (changed) {
                persistEngineSettings();
            }
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
                        applyProjectRenderSettings();
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

        bool contentDirectoryExists(const std::filesystem::path& directory) const {
            if (directory.empty()) {
                return true;
            }
            return projectSession != nullptr &&
                   std::ranges::any_of(projectSession->projectEntries(), [&](const project::ProjectEntry& entry) {
                       return entry.kind == project::ProjectEntryKind::Directory &&
                              entry.relativePath.lexically_normal() == directory.lexically_normal();
                   });
        }

        void navigateContent(std::filesystem::path directory, bool remember = true) {
            directory = directory.lexically_normal();
            if (directory == ".") {
                directory.clear();
            }
            if (!contentDirectoryExists(directory) || directory == contentDirectory) {
                return;
            }
            if (remember) {
                contentHistory.push_back(contentDirectory);
            }
            contentDirectory = std::move(directory);
        }

        void synchronizeProjectFiles(bool manual) {
            if (projectSession == nullptr || !projectSession->hasProject()) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!manual && lastContentSync.time_since_epoch().count() != 0 &&
                now - lastContentSync < std::chrono::seconds{1}) {
                return;
            }
            lastContentSync = now;
            const project::AssetSyncResult result = projectSession->synchronizeProjectFiles(manual);
            if (!result.succeeded()) {
                statusMessage = result.error;
            } else if (manual || result.changed()) {
                statusMessage = std::to_string(result.added) + " added, " + std::to_string(result.moved) + " moved, " +
                                std::to_string(result.modified) + " modified, " + std::to_string(result.missing) +
                                " missing.";
                if (!result.diagnostics.empty()) {
                    statusMessage += " " + result.diagnostics.front();
                }
            }
            if (!contentDirectoryExists(contentDirectory)) {
                contentDirectory.clear();
                contentHistory.clear();
            }
        }

        void drawDirectoryTreeNode(const std::filesystem::path& directory, std::string_view label, bool rootNode) {
            bool hasChildren = false;
            for (const project::ProjectEntry& entry : projectSession->projectEntries()) {
                if (entry.kind == project::ProjectEntryKind::Directory &&
                    entry.relativePath.parent_path() == directory) {
                    hasChildren = true;
                    break;
                }
            }
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!hasChildren) {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            if (directory == contentDirectory) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            if (rootNode) {
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            ImGui::PushID(directory.generic_string().c_str());
            const bool open =
                ImGui::TreeNodeEx("##directory", flags, "%.*s", static_cast<int>(label.size()), label.data());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                navigateContent(directory);
            }
            if (open) {
                for (const project::ProjectEntry& entry : projectSession->projectEntries()) {
                    if (entry.kind == project::ProjectEntryKind::Directory &&
                        entry.relativePath.parent_path() == directory) {
                        drawDirectoryTreeNode(entry.relativePath, entry.relativePath.filename().string(), false);
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        void drawContentTile(const project::ProjectEntry& entry, std::optional<project::AssetId>& removeAsset) {
            constexpr float tileWidth = 104.0f;
            constexpr float tileHeight = 96.0f;
            constexpr float iconSize = 46.0f;
            const project::AssetRecord* asset =
                entry.asset.has_value() ? projectSession->assets().find(*entry.asset) : nullptr;
            const std::string id = entry.relativePath.generic_string();
            ImGui::PushID(id.c_str());
            ImGui::InvisibleButton("##entry", {tileWidth, tileHeight});
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (hovered) {
                drawList->AddRectFilled(minimum, maximum, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.0f);
            }
            drawBrowserIcon(*drawList, {minimum.x + (tileWidth - iconSize) * 0.5f, minimum.y + 5.0f}, iconSize,
                            browserIconFor(entry, asset));
            const std::string name = entry.relativePath.filename().string();
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {minimum.x + 5.0f, minimum.y + 58.0f},
                              ImGui::GetColorU32(ImGuiCol_Text), name.c_str(), nullptr, tileWidth - 10.0f);

            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.kind == project::ProjectEntryKind::Directory) {
                    navigateContent(entry.relativePath);
                } else if (asset != nullptr && asset->available && asset->type == project::AssetType::Mesh) {
                    if (const auto created = projectSession->createActorFromMesh(asset->id); created.has_value()) {
                        selectActor(*created);
                    }
                }
            }
            if (hovered) {
                ImGui::SetTooltip("%s", entry.relativePath.generic_string().c_str());
            }
            if (asset != nullptr && asset->available && ImGui::BeginPopupContextItem("AssetContext")) {
                if (ImGui::MenuItem("Rename")) {
                    renameAssetId = asset->id;
                    renameAssetName.fill('\0');
                    const std::size_t length = std::min(asset->displayName.size(), renameAssetName.size() - 1);
                    std::copy_n(asset->displayName.data(), length, renameAssetName.data());
                    showRenameAsset = true;
                }
                if (ImGui::MenuItem("Delete")) {
                    removeAsset = asset->id;
                }
                ImGui::EndPopup();
            }
            if (asset != nullptr && asset->available && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("LUMIN_ASSET", asset->id.value.data(), asset->id.value.size());
                ImGui::TextUnformatted(asset->displayName.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }

        void drawContentBrowser() {
            if (!ImGui::Begin("Content Browser", &engineSettings.windows.contentBrowser)) {
                ImGui::End();
                return;
            }
            if (projectSession == nullptr || !projectSession->hasProject()) {
                contentProjectRoot.clear();
                contentDirectory.clear();
                contentHistory.clear();
                ImGui::TextDisabled("Open or create a project to browse content.");
                ImGui::End();
                return;
            }

            if (contentProjectRoot != projectSession->rootDirectory()) {
                contentProjectRoot = projectSession->rootDirectory();
                contentDirectory.clear();
                contentHistory.clear();
                contentSearch.fill('\0');
                lastContentSync = std::chrono::steady_clock::now();
            }
            synchronizeProjectFiles(false);

            if (toolbarIconButton("##contentBack", ToolbarIcon::Back, !contentHistory.empty())) {
                contentDirectory = contentHistory.back();
                contentHistory.pop_back();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Back");
            }
            ImGui::SameLine();
            if (toolbarIconButton("##contentUp", ToolbarIcon::Up, !contentDirectory.empty())) {
                navigateContent(contentDirectory.parent_path());
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Up");
            }
            ImGui::SameLine();
            if (toolbarIconButton("##contentRefresh", ToolbarIcon::Refresh)) {
                synchronizeProjectFiles(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Refresh");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(projectSession->manifest().name.c_str())) {
                navigateContent({});
            }
            const std::filesystem::path displayedDirectory = contentDirectory;
            std::filesystem::path breadcrumb;
            std::optional<std::filesystem::path> breadcrumbTarget;
            for (const auto& component : displayedDirectory) {
                breadcrumb /= component;
                ImGui::SameLine();
                ImGui::TextDisabled(">");
                ImGui::SameLine();
                const std::string label = component.string() + "##" + breadcrumb.generic_string();
                if (ImGui::SmallButton(label.c_str())) {
                    breadcrumbTarget = breadcrumb;
                }
            }
            if (breadcrumbTarget.has_value()) {
                navigateContent(std::move(*breadcrumbTarget));
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##contentSearch", "Search project", contentSearch.data(), contentSearch.size());

            const float treeWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.24f, 150.0f, 260.0f);
            ImGui::BeginChild("DirectoryTree", {treeWidth, -ImGui::GetFrameHeightWithSpacing()}, false);
            drawDirectoryTreeNode({}, projectSession->manifest().name, true);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("DirectoryContents", {0.0f, -ImGui::GetFrameHeightWithSpacing()}, false);

            std::vector<const project::ProjectEntry*> visibleEntries;
            const std::string query = lowerAscii(contentSearch.data());
            for (const project::ProjectEntry& entry : projectSession->projectEntries()) {
                const bool visible =
                    query.empty()
                        ? entry.relativePath.parent_path() == contentDirectory
                        : pathWithin(entry.relativePath, contentDirectory) &&
                              lowerAscii(entry.relativePath.filename().string()).find(query) != std::string::npos;
                if (visible) {
                    visibleEntries.push_back(&entry);
                }
            }
            std::ranges::sort(visibleEntries, [](const auto* left, const auto* right) {
                if (left->kind != right->kind) {
                    return left->kind == project::ProjectEntryKind::Directory;
                }
                return lowerAscii(left->relativePath.filename().string()) <
                       lowerAscii(right->relativePath.filename().string());
            });

            constexpr float tileWidth = 104.0f;
            const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / tileWidth));
            std::optional<project::AssetId> removeAsset;
            if (ImGui::BeginTable("ProjectEntries", columns, ImGuiTableFlags_SizingFixedFit)) {
                for (const project::ProjectEntry* entry : visibleEntries) {
                    ImGui::TableNextColumn();
                    drawContentTile(*entry, removeAsset);
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();

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
                ImGui::TextUnformatted(statusMessage.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", statusMessage.c_str());
                }
            }
            ImGui::End();
        }

        void drawHierarchy() {
            ImGui::Begin("Scene Hierarchy", &engineSettings.windows.sceneHierarchy);
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
                            if (asset.available && asset.type == project::AssetType::Script &&
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
                scene::PbrTextureSet textureDraft = material.textures.value_or(scene::PbrTextureSet{});
                bool textureChanged = false;
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
                                record != nullptr && record->available && record->type == project::AssetType::Texture) {
                                target = projectSession->rootDirectory() / record->relativePath;
                                textureChanged = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                };
                textureSlot("Base color", "##baseColorTexture", textureDraft.baseColor);
                textureSlot("Normal", "##normalTexture", textureDraft.normal);
                textureSlot("Roughness", "##roughnessTexture", textureDraft.roughness);
                if (textureChanged) {
                    material.textures = std::move(textureDraft);
                    changed = true;
                }
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
            ImGui::Begin("Details", &engineSettings.windows.details);
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
                            if (asset.available && asset.type == project::AssetType::Script &&
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
            const bool drawContents = ImGui::Begin("Viewport", &engineSettings.windows.viewport, flags);
            if (!drawContents || !engineSettings.windows.viewport) {
                viewportInteraction = {};
                ImGui::End();
                ImGui::PopStyleVar();
                return;
            }
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
            char fpsLabel[32]{};
            std::snprintf(fpsLabel, sizeof(fpsLabel), "FPS %.1f", ImGui::GetIO().Framerate);
            const float fpsWidth = ImGui::CalcTextSize(fpsLabel).x;
            const float fpsPosition = ImGui::GetWindowContentRegionMax().x - fpsWidth - style::Space2;
            const float toolbarEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            if (fpsPosition > toolbarEnd + style::Space2) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(fpsPosition);
                ImGui::TextDisabled("%s", fpsLabel);
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
                ImGui::Image(static_cast<ImTextureID>(image.textureId.value()), available);
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
            ImGui::Begin("Render / GI", &engineSettings.windows.renderSettings);
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
                ImGui::Checkbox("Ambient occlusion", &settings.globalIllumination.ssaoEnabled);
                if (settings.globalIllumination.ssaoEnabled) {
                    propertyLabel("Algorithm");
                    if (ImGui::BeginCombo("##ambientOcclusionMode",
                                          ambientOcclusionModeName(settings.globalIllumination.ambientOcclusionMode))) {
                        for (const auto [mode, name] : {
                                 std::pair{render::AmbientOcclusionMode::Ssao, "SSAO"},
                                 std::pair{render::AmbientOcclusionMode::Hbao, "HBAO"},
                                 std::pair{render::AmbientOcclusionMode::Gtao, "GTAO"},
                             }) {
                            const bool selected = settings.globalIllumination.ambientOcclusionMode == mode;
                            if (ImGui::Selectable(name, selected)) {
                                settings.globalIllumination.ambientOcclusionMode = mode;
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    propertyLabel("Radius");
                    ImGui::SliderFloat("##ambientOcclusionRadius", &settings.globalIllumination.ambientOcclusionRadius,
                                       0.05f, 5.0f, "%.2f");
                    propertyLabel("Strength");
                    ImGui::SliderFloat("##ambientOcclusionStrength",
                                       &settings.globalIllumination.ambientOcclusionStrength, 0.0f, 4.0f, "%.2f");
                    propertyLabel("Bias");
                    ImGui::SliderFloat("##ambientOcclusionBias", &settings.globalIllumination.ambientOcclusionBias,
                                       0.0f, 0.5f, "%.3f");
                }
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
            ImGui::Begin("Script Console", &engineSettings.windows.scriptConsole);
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
                   ViewportImageProvider viewportImage, project::ProjectSession* project, EditorDialogServices dialogs,
                   EditorSettingsServices settingsServices)
        : impl_(std::make_unique<Impl>(level, camera, settings, scripts, std::move(backendInfo),
                                       std::move(viewportImage), project, std::move(dialogs),
                                       std::move(settingsServices))) {
    }

    Editor::~Editor() = default;
    Editor::Editor(Editor&&) noexcept = default;
    Editor& Editor::operator=(Editor&&) noexcept = default;

    void Editor::draw() {
        style::apply();
        synchronizeSelection();
        ImGuizmo::BeginFrame();
        const config::EditorWindowVisibility visibilityBefore = impl_->engineSettings.windows;
        impl_->drawMainMenu();
        if (impl_->projectSession != nullptr && !impl_->projectSession->hasProject()) {
            impl_->viewportInteraction = {};
            impl_->drawProjectNavigator();
        } else {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImGuiID dockspace = ImGui::GetID("LuminEditorDockspace");
            impl_->buildLayout(dockspace, *viewport);
            ImGui::DockSpaceOverViewport(dockspace, viewport);
            auto& windows = impl_->engineSettings.windows;
            if (windows.sceneHierarchy) {
                impl_->drawHierarchy();
            }
            if (windows.contentBrowser) {
                impl_->drawContentBrowser();
            }
            if (windows.details) {
                impl_->drawInspector();
            }
            if (windows.viewport) {
                impl_->drawViewport();
            } else {
                impl_->viewportInteraction = {};
            }
            if (windows.renderSettings) {
                impl_->drawRenderSettings();
            }
            if (windows.scriptConsole) {
                impl_->drawConsole();
            }
        }
        impl_->drawConfiguration();
        impl_->drawProjectModals();
        if (visibilityBefore != impl_->engineSettings.windows) {
            impl_->persistEngineSettings();
        }
        if (impl_->projectSession != nullptr && impl_->projectSession->hasProject() && ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_S, false)) {
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

    void Editor::setAmbientOcclusionMode(render::AmbientOcclusionMode mode) noexcept {
        impl_->settings.globalIllumination.ambientOcclusionMode = mode;
    }

    void Editor::setAmbientOcclusionRadius(float radius) noexcept {
        impl_->settings.globalIllumination.ambientOcclusionRadius = std::max(radius, 0.05f);
    }

    void Editor::setAmbientOcclusionStrength(float strength) noexcept {
        impl_->settings.globalIllumination.ambientOcclusionStrength = std::max(strength, 0.0f);
    }

    void Editor::setAmbientOcclusionBias(float bias) noexcept {
        impl_->settings.globalIllumination.ambientOcclusionBias = std::clamp(bias, 0.0f, 0.5f);
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

    bool Editor::openProject(const std::filesystem::path& path) {
        return impl_->openProjectPath(path);
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

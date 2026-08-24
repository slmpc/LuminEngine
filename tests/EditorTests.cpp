#include "render/editor/Editor.hpp"
#include "render/editor/EditorLayout.hpp"
#include "render/editor/EditorStyle.hpp"

#include "assets/ObjLoader.hpp"
#include "render/editor/ImGuiContent.hpp"
#include "render/editor/ImGuiFrontend.hpp"
#include "render/editor/RenderSettingsPanelAdapter.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

namespace {

    class TestActor final : public lumin::scene::Actor {};

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("lumin-editor-tests-" + std::to_string(suffix));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool nearlyEqual(float left, float right) {
        return std::abs(left - right) < 0.0001f;
    }

    lumin::assets::Mesh makeTriangle() {
        lumin::assets::Mesh mesh;
        mesh.name = "editor-test";
        mesh.vertices.resize(3);
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    void writeText(const std::filesystem::path& path, std::string_view text) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
        if (!stream) {
            throw std::runtime_error("Failed to prepare editor test input.");
        }
    }

    struct TestLogicState {
        TestLogicState(lumin::scene::Level& levelValue, lumin::scene::Camera& cameraValue,
                       lumin::scripting::ScriptRuntime& scriptsValue, lumin::project::ProjectSession* projectValue)
            : level(levelValue), camera(cameraValue), scripts(scriptsValue), externalProject(projectValue) {
            if (externalProject == nullptr) {
                ownedProject = std::make_unique<lumin::project::ProjectSession>(level, camera, scripts);
            }
        }

        [[nodiscard]] lumin::project::ProjectSession& project() const noexcept {
            return externalProject != nullptr ? *externalProject : *ownedProject;
        }

        [[nodiscard]] std::shared_ptr<const lumin::editor::EditorLogicSnapshot> capture() {
            auto result = std::make_shared<lumin::editor::EditorLogicSnapshot>();
            result->revision = nextSnapshotRevision++;
            result->renderWorld = lumin::render::world::RenderWorldExtractor::extract(level);
            result->camera = camera;
            result->environment = level.environment();
            for (const lumin::scene::ActorHandle handle : level.actorHandles()) {
                if (const lumin::scene::Actor* actor = level.actor(handle); actor != nullptr) {
                    result->actors.push_back({handle, actor->name(), actor->transform(), actor->material(),
                                              actor->modelHandle(), actor->localLight()});
                }
            }
            for (const lumin::scene::ModelHandle handle : level.modelHandles()) {
                const lumin::scene::ModelInstance& model = level.model(handle);
                result->models.push_back(
                    {handle, model, level.actorForModel(handle), project().assetForMesh(model.mesh)});
            }
            result->scripts = scripts.scripts();
            result->scriptDiagnostics = scripts.diagnostics();
            result->consoleHistory = scripts.consoleHistory();
            result->project.open = project().hasProject();
            result->project.dirty = project().dirty();
            result->project.projectFile = project().projectFile();
            result->project.rootDirectory = project().rootDirectory();
            result->project.manifest = project().manifest();
            result->project.assets = project().assets();
            result->project.entries = project().projectEntries();
            result->project.diagnostics = project().diagnostics();
            result->project.settings = project().settings();
            return result;
        }

        lumin::scene::Level& level;
        lumin::scene::Camera& camera;
        lumin::scripting::ScriptRuntime& scripts;
        lumin::project::ProjectSession* externalProject = nullptr;
        std::unique_ptr<lumin::project::ProjectSession> ownedProject;
        std::vector<lumin::editor::EditorCommandResult> results;
        lumin::editor::EditorCommandId nextCommandId = 1;
        std::uint64_t nextSnapshotRevision = 1;
    };

    lumin::editor::EditorLogicServices makeLogicServices(lumin::scene::Level& level, lumin::scene::Camera& camera,
                                                         lumin::scripting::ScriptRuntime& scripts,
                                                         lumin::project::ProjectSession* project = nullptr) {
        auto state = std::make_shared<TestLogicState>(level, camera, scripts, project);
        return {
            .snapshot =
                [state] {
                    return state->capture();
                },
            .submit =
                [state](lumin::editor::EditorLogicCommand command) {
                    const lumin::editor::EditorCommandId id = state->nextCommandId++;
                    state->results.push_back(
                        {id, command(state->level, state->camera, state->scripts, state->project())});
                    return id;
                },
            .drainResults =
                [state] {
                    std::vector<lumin::editor::EditorCommandResult> results;
                    results.swap(state->results);
                    return results;
                },
        };
    }

    lumin::editor::Editor makeEditor(lumin::scene::Level& level, lumin::scene::Camera& camera,
                                     lumin::render::RenderSettings& settings, lumin::scripting::ScriptRuntime& runtime,
                                     lumin::editor::ViewportImageProvider viewportImage = {},
                                     lumin::project::ProjectSession* project = nullptr,
                                     lumin::editor::EditorCameraServices cameraServices = {}) {
        return lumin::editor::Editor{makeLogicServices(level, camera, runtime, project),
                                     settings,
                                     [] {
                                         return lumin::render::gi::BackendInfo{"SSAO", false, false};
                                     },
                                     std::move(viewportImage),
                                     {},
                                     {},
                                     {},
                                     std::move(cameraServices)};
    }

    void testEmptyAndStaleSelection() {
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime;
        auto editor = makeEditor(level, camera, settings, runtime);

        require(editor.selectionState() == lumin::editor::SelectionState::Empty,
                "A new editor must have an explicit empty selection.");

        const auto actor = level.spawnActor<TestActor>();
        require(editor.selectActor(actor), "A live actor must be selectable.");
        require(level.destroyActor(actor), "The selected actor must be removable.");
        editor.synchronizeSelection();
        require(editor.selectionState() == lumin::editor::SelectionState::Stale && !editor.selectedActor().has_value(),
                "A stale actor selection must clear its writable handle.");

        const auto mesh = level.addMesh(makeTriangle());
        const auto model = level.addModel(mesh);
        require(editor.selectModel(model), "A live model must be selectable.");
        require(level.removeModel(model), "The selected model must be removable.");
        editor.synchronizeSelection();
        require(editor.selectionState() == lumin::editor::SelectionState::Stale && !editor.selectedModel().has_value(),
                "A stale model selection must clear its writable handle.");
    }

    void testExactInputCapturePolicy() {
        using lumin::render::ImGuiCaptureState;
        require(!ImGuiCaptureState{}.uiClaimsInput(), "No capture flag must leave gameplay input available.");
        require(ImGuiCaptureState{.wantCaptureKeyboard = true}.uiClaimsInput(),
                "Keyboard capture must claim gameplay input.");
        require(ImGuiCaptureState{.wantCaptureMouse = true}.uiClaimsInput(),
                "Mouse capture must claim gameplay input.");
        require(ImGuiCaptureState{.wantTextInput = true}.uiClaimsInput(),
                "Text input capture must claim gameplay input.");
        require(ImGuiCaptureState{true, true, true}.uiClaimsInput(),
                "Combined capture flags must continue to claim gameplay input.");
    }

    void testRenderSettingsPanelAdapterPublishesTypedSnapshots() {
        lumin::render::editor::RenderSettingsPanelAdapter adapter;
        const lumin::render::core::RenderSettingsSnapshot before = adapter.snapshot();
        adapter.editable().toneMapping.exposure = 2.5f;
        adapter.editable().toneMapping.agxEnabled = false;
        adapter.editable().toneMapping.autoExposureEnabled = false;
        adapter.editable().toneMapping.exposureCompensationEv = 1.25f;
        adapter.editable().bloom.enabled = false;
        adapter.editable().bloom.intensity = 0.2f;
        adapter.editable().temporalAa.sharpness = 0.8f;
        adapter.editable().globalIllumination.mode = lumin::render::GlobalIlluminationMode::Legacy;
        const lumin::render::core::RenderSettingsSnapshot after = adapter.snapshot();

        require(before.get<lumin::render::ToneMappingSettings>(lumin::render::pipelines::feature_ids::toneMapping())
                        .exposure == 1.0f,
                "An older settings snapshot must not observe later panel edits.");
        require(after.get<lumin::render::ToneMappingSettings>(lumin::render::pipelines::feature_ids::toneMapping())
                            .exposure == 2.5f &&
                    !after.get<lumin::render::ToneMappingSettings>(lumin::render::pipelines::feature_ids::toneMapping())
                         .agxEnabled &&
                    !after.get<lumin::render::ToneMappingSettings>(lumin::render::pipelines::feature_ids::toneMapping())
                         .autoExposureEnabled &&
                    after.get<lumin::render::ToneMappingSettings>(lumin::render::pipelines::feature_ids::toneMapping())
                            .exposureCompensationEv == 1.25f &&
                    !after.get<lumin::render::BloomSettings>(lumin::render::pipelines::feature_ids::bloom()).enabled &&
                    after.get<lumin::render::BloomSettings>(lumin::render::pipelines::feature_ids::bloom()).intensity ==
                        0.2f &&
                    after.get<lumin::render::TemporalAaSettings>(lumin::render::pipelines::feature_ids::temporalAa())
                            .sharpness == 0.8f &&
                    after.get<lumin::render::GlobalIlluminationSettings>(
                             lumin::render::pipelines::feature_ids::globalIllumination())
                            .mode == lumin::render::GlobalIlluminationMode::Legacy,
                "The panel adapter must publish edits through the typed Feature store.");
    }

    void testConsoleReturnsValues() {
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime;
        auto editor = makeEditor(level, camera, settings, runtime);

        const auto result = editor.executeCommand("return 6 * 7");
        require(result.succeeded && result.values.size() == 1 && result.values.front() == "42",
                "The editor console must expose Lua return values.");
        const auto entries = editor.consoleEntries();
        require(!entries.empty() && entries.back().message == "42",
                "The visible console model must preserve the returned value.");
    }

    void testLayoutLifecycleTransitions() {
        using lumin::editor::EditorLayoutLifecycle;
        using lumin::editor::EditorLayoutMode;
        using lumin::editor::editorLayoutModeForExtent;
        using lumin::editor::editorLayoutModeForViewportSize;

        require(editorLayoutModeForExtent(1280.0f, 720.0f) == EditorLayoutMode::Full,
                "The design-system minimum extent must use the full layout.");
        require(editorLayoutModeForExtent(1279.0f, 720.0f) == EditorLayoutMode::Compact &&
                    editorLayoutModeForExtent(1280.0f, 719.0f) == EditorLayoutMode::Compact,
                "Crossing either minimum extent must select compact layout.");
        require(editorLayoutModeForViewportSize(1280.0f, 720.0f) == EditorLayoutMode::Full &&
                    editorLayoutModeForViewportSize(1276.0f, 711.0f) == EditorLayoutMode::Full,
                "A native 1280x720 viewport must remain full when constrained by desktop chrome.");
        require(editorLayoutModeForViewportSize(1275.0f, 711.0f) == EditorLayoutMode::Compact &&
                    editorLayoutModeForViewportSize(1200.0f, 680.0f) == EditorLayoutMode::Compact,
                "Viewports smaller than the tolerated desktop size must use compact layout.");

        EditorLayoutLifecycle lifecycle;
        const auto* firstContext = reinterpret_cast<const void*>(1);
        const auto* secondContext = reinterpret_cast<const void*>(2);
        require(lifecycle.update(firstContext, EditorLayoutMode::Full, 1),
                "The first valid context must build a layout.");
        require(!lifecycle.update(firstContext, EditorLayoutMode::Full, 1),
                "An unchanged context, mode, and schema must preserve user docking.");
        require(lifecycle.update(firstContext, EditorLayoutMode::Compact, 1) &&
                    lifecycle.update(firstContext, EditorLayoutMode::Full, 1),
                "Full to compact to full transitions must each rebuild once.");
        require(lifecycle.update(secondContext, EditorLayoutMode::Full, 1),
                "A recreated ImGui context must rebuild the named dock layout.");
        require(lifecycle.update(secondContext, EditorLayoutMode::Full, 2),
                "A layout schema change must rebuild the named dock layout.");
    }

    void testEditorNeutralDarkTheme() {
        ImGui::CreateContext();
        lumin::editor::style::apply();
        const ImVec4* colors = ImGui::GetStyle().Colors;
        const ImVec4& window = colors[ImGuiCol_WindowBg];
        const ImVec4& frame = colors[ImGuiCol_FrameBg];
        const ImVec4& hovered = colors[ImGuiCol_FrameBgHovered];
        const ImVec4& checkMark = colors[ImGuiCol_CheckMark];

        require(nearlyEqual(window.x, 25.0f / 255.0f) && nearlyEqual(window.y, 27.0f / 255.0f) &&
                    nearlyEqual(window.z, 29.0f / 255.0f),
                "The editor window must use the neutral near-black reference palette.");
        require(frame.x > window.x && hovered.x > frame.x &&
                    colors[ImGuiCol_Border].x < colors[ImGuiCol_TextDisabled].x,
                "Frames, hover states, and borders must preserve readable dark-surface hierarchy.");
        require(checkMark.x > checkMark.y && checkMark.y > checkMark.z &&
                    nearlyEqual(colors[ImGuiCol_SliderGrab].x, checkMark.x),
                "Sparse interactive accents must use the shared warm highlight color.");
        ImGui::DestroyContext();
    }

    void testDockLayoutSkipsNonpositiveWorkSizeAndBuildsAfterRestore() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = {1280.0f, 720.0f};
        ImGui::ClearIniSettings();
        unsigned char* fontPixels = nullptr;
        int fontWidth = 0;
        int fontHeight = 0;
        io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
        require(fontPixels != nullptr && fontWidth > 0 && fontHeight > 0,
                "The docking regression requires a built ImGui font atlas.");

        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime;
        TemporaryDirectory temporary;
        lumin::project::ProjectSession project(level, camera, runtime);
        std::string error;
        require(project.create(temporary.path, "DockLayout", error), error.c_str());
        bool viewportImageRequested = false;
        auto editor = makeEditor(
            level, camera, settings, runtime,
            [&viewportImageRequested] {
                viewportImageRequested = true;
                return lumin::render::ImGuiViewportImage{lumin::render::core::UiTextureId{0x1234U}, 640, 360};
            },
            &project);

        const auto drawFrame = [&editor](const ImVec2 workSize) {
            ImGui::NewFrame();
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            viewport->WorkPos = {0.0f, 0.0f};
            viewport->WorkSize = workSize;
            editor.draw();
            ImGui::EndFrame();
        };

        drawFrame({0.0f, 0.0f});
        drawFrame({-1.0f, -1.0f});
        drawFrame({1280.0f, 720.0f});

        const ImGuiWindow* hierarchy = ImGui::FindWindowByName("Scene Hierarchy");
        require(hierarchy != nullptr && hierarchy->DockId != 0,
                "A restored positive viewport must build the production docking layout after skipped invalid extents.");
        const ImGuiWindow* contentBrowser = ImGui::FindWindowByName("Content Browser");
        const ImGuiWindow* console = ImGui::FindWindowByName("Script Console");
        const ImGuiWindow* details = ImGui::FindWindowByName("Details");
        const ImGuiWindow* renderSettings = ImGui::FindWindowByName("Render / GI");
        require(contentBrowser != nullptr && console != nullptr && contentBrowser->DockId == console->DockId,
                "Content Browser and Script Console must share the bottom tab group.");
        require(details != nullptr && renderSettings != nullptr && details->DockId == renderSettings->DockId,
                "Details and Render / GI must share the lower-right tab group.");
        const ImGuiWindow* viewportWindow = ImGui::FindWindowByName("Viewport");
        require(viewportWindow != nullptr && viewportWindow->DockId != 0 && viewportImageRequested,
                "Viewport must be an independent dock window backed by the renderer image provider.");
        require(hierarchy->DockId != details->DockId && hierarchy->Pos.x > viewportWindow->Pos.x &&
                    details->Pos.x > viewportWindow->Pos.x && contentBrowser->Pos.x == viewportWindow->Pos.x,
                "Scene Hierarchy must occupy the upper-right area while the bottom tabs align below Viewport.");
        const auto viewportState = editor.viewportInteraction();
        const std::uint32_t expectedWidth = static_cast<std::uint32_t>(
            std::max(viewportWindow->ContentRegionRect.GetWidth() * io.DisplayFramebufferScale.x, 1.0f));
        const std::uint32_t expectedHeight = static_cast<std::uint32_t>(
            std::max((viewportWindow->ContentRegionRect.GetHeight() - ImGui::GetFrameHeightWithSpacing()) *
                         io.DisplayFramebufferScale.y,
                     1.0f));
        require(viewportState.width == expectedWidth && viewportState.height == expectedHeight,
                "Viewport interaction extent must match the dock content region in physical pixels.");
        ImGui::DestroyContext();
    }

    void testConsoleHistoryAndClearScopes() {
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime;
        auto editor = makeEditor(level, camera, settings, runtime);

        require(editor.executeCommand("return 1").succeeded && editor.executeCommand("return 2").succeeded,
                "History setup commands must execute.");
        require(editor.commandHistoryPrevious("draft") == "return 2" &&
                    editor.commandHistoryPrevious("ignored") == "return 1",
                "Up navigation must walk command history newest to oldest.");
        require(editor.commandHistoryNext() == "return 2" && editor.commandHistoryNext() == "draft",
                "Down navigation must restore the draft at the end of history.");

        require(!editor.executeCommand("error('review failure')").succeeded,
                "The diagnostic clear test requires a failed command.");
        const std::size_t historyBeforeClear = runtime.consoleHistory().size();
        editor.clearDiagnostics();
        require(runtime.consoleHistory().size() == historyBeforeClear,
                "Clear Diagnostics must not erase command history.");
        const std::size_t visibleAfterDiagnosticsClear = editor.consoleEntries().size();
        editor.clearCommandHistory();
        require(runtime.consoleHistory().empty(), "Clear History must erase runtime command history.");
        require(editor.consoleEntries().size() == visibleAfterDiagnosticsClear,
                "Clear History must not erase visible command results.");
    }

    void testFailedCommandAppearsOnce() {
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime;
        auto editor = makeEditor(level, camera, settings, runtime);

        require(!editor.executeCommand("error('single failure')").succeeded,
                "The failed-command regression requires an execution error.");
        editor.synchronizeConsole();
        editor.synchronizeConsole();
        const auto errorCount = std::count_if(editor.consoleEntries().begin(), editor.consoleEntries().end(),
                                              [](const lumin::editor::ConsoleEntry& entry) {
                                                  return entry.severity == lumin::scripting::ScriptSeverity::Error &&
                                                         entry.message.find("single failure") != std::string::npos;
                                              });
        require(errorCount == 1, "A failed command must produce exactly one visible console error.");
    }

    void testUninitializedFrontendHasNoThreadState() {
        lumin::render::ImGuiFrontend frontend;
        require(!frontend.initialized() && !frontend.frameActive(),
                "A default ImGui frontend must not own a context or active frame.");
        require(!frontend.captureState().uiClaimsInput(),
                "An uninitialized frontend must expose an empty capture state.");
    }

    void testSettingsAndSelectionMutation() {
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime;
        auto editor = makeEditor(level, camera, settings, runtime);

        editor.setCameraSpeed(8.0f);
        editor.setCameraPosition({1.0f, 2.0f, 3.0f});
        editor.setDirectLightingEnabled(false);
        editor.setShadowsEnabled(false);
        editor.setGlobalIlluminationMode(lumin::render::GlobalIlluminationMode::Legacy);
        editor.setSsaoEnabled(false);
        editor.setAmbientOcclusionMode(lumin::render::AmbientOcclusionMode::Hbao);
        editor.setAmbientOcclusionRadius(-1.0f);
        editor.setAmbientOcclusionStrength(-1.0f);
        editor.setAmbientOcclusionBias(2.0f);
        editor.setSharcEnabled(false);
        editor.setNrdEnabled(false);
        editor.setCsmSplitLambda(1.5f);
        editor.setCsmMaxDistance(-10.0f);
        editor.setTaaEnabled(false);
        editor.setTaaSharpness(2.0f);
        editor.setExposure(2.25f);
        editor.setSunDirection({0.0f, -1.0f, 0.0f});
        require(nearlyEqual(camera.moveSpeed(), 8.0f) && camera.position() == glm::vec3(1.0f, 2.0f, 3.0f),
                "Camera controls must mutate the borrowed camera.");
        require(!settings.directLighting.enabled && !settings.shadows.enabled &&
                    settings.globalIllumination.mode == lumin::render::GlobalIlluminationMode::Legacy &&
                    !settings.globalIllumination.ssaoEnabled &&
                    settings.globalIllumination.ambientOcclusionMode == lumin::render::AmbientOcclusionMode::Hbao &&
                    nearlyEqual(settings.globalIllumination.ambientOcclusionRadius, 0.05f) &&
                    nearlyEqual(settings.globalIllumination.ambientOcclusionStrength, 0.0f) &&
                    nearlyEqual(settings.globalIllumination.ambientOcclusionBias, 0.5f) &&
                    !settings.globalIllumination.sharcEnabled && !settings.globalIllumination.nrdEnabled &&
                    nearlyEqual(settings.shadows.splitLambda, 1.0f) &&
                    nearlyEqual(settings.shadows.maxDistance, 1.0f) && !settings.temporalAa.enabled &&
                    nearlyEqual(settings.temporalAa.sharpness, 1.0f) &&
                    nearlyEqual(settings.toneMapping.exposure, 2.25f) &&
                    level.environment().sun.direction == glm::vec3(0.0f, -1.0f, 0.0f) && camera.cutEpoch() == 1,
                "Legacy/RT lighting controls must mutate mode-specific settings and shared TAA state.");

        const auto actor = level.spawnActor<TestActor>();
        require(editor.selectActor(actor), "A live actor must be selectable for Inspector edits.");
        lumin::scene::Transform actorTransform;
        actorTransform.position = {4.0f, 5.0f, 6.0f};
        require(editor.setSelectedTransform(actorTransform) &&
                    level.actor(actor)->transform().position == glm::vec3(4.0f, 5.0f, 6.0f),
                "Inspector actor transforms must use the Level-owned actor API.");

        const auto mesh = level.addMesh(makeTriangle());
        const auto model = level.addModel(mesh);
        require(editor.selectModel(model), "A live model must be selectable for Inspector edits.");
        lumin::scene::Material material;
        material.metallicRoughness.roughness = 0.75f;
        require(editor.setSelectedMaterial(material) &&
                    nearlyEqual(level.model(model).material.metallicRoughness.roughness, 0.75f),
                "Inspector model materials must use the Level mutation API.");
    }

    void testCameraControlsUseRenderThreadService() {
        lumin::scene::Level level;
        lumin::scene::Camera logicCamera;
        lumin::scene::Camera viewportCamera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime;
        std::uint32_t cameraUpdates = 0;
        auto editor = makeEditor(
            level, logicCamera, settings, runtime, {}, nullptr,
            lumin::editor::EditorCameraServices{.snapshot =
                                                    [&viewportCamera] {
                                                        return viewportCamera;
                                                    },
                                                .update =
                                                    [&viewportCamera, &cameraUpdates](lumin::scene::Camera camera) {
                                                        viewportCamera = std::move(camera);
                                                        ++cameraUpdates;
                                                    }});

        editor.setCameraSpeed(9.0f);
        editor.setCameraPosition({3.0f, 4.0f, 5.0f});
        require(nearlyEqual(viewportCamera.moveSpeed(), 9.0f) &&
                    viewportCamera.position() == glm::vec3(3.0f, 4.0f, 5.0f) && viewportCamera.cutEpoch() == 1 &&
                    cameraUpdates == 2,
                "Camera controls must update the render-thread Camera service immediately.");
        require(!nearlyEqual(logicCamera.moveSpeed(), 9.0f) && logicCamera.position() != glm::vec3(3.0f, 4.0f, 5.0f),
                "Camera controls must not mutate the stale logic snapshot when a render Camera service exists.");
    }

    void testLocalLightEditingAndActorLifecycle() {
        TemporaryDirectory temporary;
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings settings;
        lumin::scripting::ScriptRuntime runtime({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, runtime);
        std::string error;
        require(project.create(temporary.path, "EditorLocalLights", error), error.c_str());

        const std::filesystem::path meshPath = project.rootDirectory() / "Models/light-fixture.obj";
        std::filesystem::create_directories(meshPath.parent_path());
        writeText(meshPath, "v -1 0 0\nv 1 0 0\nv 0 2 0\nf 1 2 3\n");
        require(project.synchronizeProjectFiles(true).succeeded(), "The fixture mesh must be discoverable.");
        const lumin::project::AssetRecord* meshAsset = project.assets().findByPath("Models/light-fixture.obj");
        require(meshAsset != nullptr, "The local-light duplication test requires a project mesh asset.");

        auto editor = makeEditor(level, camera, settings, runtime, {}, &project);
        lumin::scene::PointLight point;
        point.color = {0.8f, 0.4f, 0.2f};
        point.luminousIntensityCandela = 2400.0f;
        point.range = 18.0f;
        point.castsShadows = false;
        lumin::scene::Transform transform;
        transform.position = {2.0f, 3.0f, 4.0f};
        require(editor.createLightActor(point, transform), "The editor must create a Point Light actor.");
        editor.synchronizeSelection();
        require(editor.selectedActor().has_value() && level.actorCount() == 1 && project.dirty(),
                "Creating a local light must select it and mark the project dirty.");

        const lumin::scene::ActorHandle sourceHandle = *editor.selectedActor();
        lumin::scene::Actor* source = level.actor(sourceHandle);
        const auto mesh = project.meshForAsset(meshAsset->id);
        require(source != nullptr && mesh.has_value(), "The created light actor and fixture mesh must remain alive.");
        source->attachModel(*mesh);
        project.markDirty();
        require(project.save(error), error.c_str());

        lumin::scene::SpotLight spot;
        spot.enabled = false;
        spot.color = {0.1f, 0.3f, 0.9f};
        spot.luminousIntensityCandela = 3200.0f;
        spot.range = 24.0f;
        spot.castsShadows = true;
        spot.innerConeAngleDegrees = 15.0f;
        spot.outerConeAngleDegrees = 42.0f;
        require(editor.setSelectedLocalLight(spot) && project.dirty() &&
                    std::holds_alternative<lumin::scene::SpotLight>(*source->localLight()),
                "Details edits must switch light type, preserve valid parameters, and mark the project dirty.");
        lumin::scene::SpotLight invalid = spot;
        invalid.innerConeAngleDegrees = 50.0f;
        invalid.outerConeAngleDegrees = 40.0f;
        require(!editor.setSelectedLocalLight(invalid) && *source->localLight() == lumin::scene::LocalLight{spot},
                "The editor must reject invalid Spot cone parameters without mutating the Actor.");
        require(project.save(error), error.c_str());

        require(editor.duplicateSelectedActor(), "A model-plus-light Actor must be duplicable.");
        editor.synchronizeSelection();
        require(editor.selectedActor().has_value() && *editor.selectedActor() != sourceHandle &&
                    level.actorCount() == 2 && project.dirty(),
                "Duplicating a light Actor must select the copy and mark the project dirty.");
        lumin::scene::Actor* copy = level.actor(*editor.selectedActor());
        require(copy != nullptr && copy->modelHandle().isValid() && copy->localLight() == source->localLight() &&
                    copy->transform().position == source->transform().position + glm::vec3(0.5f, 0.0f, 0.0f),
                "Actor duplication must preserve both model and light while offsetting the copy.");

        require(project.save(error), error.c_str());
        require(editor.clearSelectedLocalLight() && !copy->localLight().has_value() && project.dirty(),
                "Removing a local light in Details must leave the model attached and mark the project dirty.");
        require(project.save(error), error.c_str());
        require(editor.deleteSelectedActor() && level.actorCount() == 1 && project.dirty(),
                "Deleting the selected light Actor must use normal Actor lifecycle and dirty handling.");
    }

    void testProjectNavigatorAndPersistedWindowVisibility() {
        TemporaryDirectory temporary;
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = {1280.0f, 720.0f};
        unsigned char* fontPixels = nullptr;
        int fontWidth = 0;
        int fontHeight = 0;
        io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);

        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::render::RenderSettings renderSettings;
        lumin::scripting::ScriptRuntime runtime({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, runtime);
        lumin::config::EngineSettings persisted;
        const auto missingProject = temporary.path / "Missing/Missing.luminproject";
        persisted.startupDestination = lumin::config::StartupDestination::LastProject;
        persisted.lastProject = missingProject;
        persisted.recentProjects.push_back(missingProject);
        persisted.windows.viewport = false;
        int saveCount = 0;

        lumin::editor::Editor editor{
            makeLogicServices(level, camera, runtime, &project),
            renderSettings,
            [] {
                return lumin::render::gi::BackendInfo{"SSAO", false, false};
            },
            {},
            {},
            lumin::editor::EditorSettingsServices{
                .settings = persisted,
                .save = [&persisted, &saveCount](const lumin::config::EngineSettings& settings, std::string&) {
                    persisted = settings;
                    ++saveCount;
                    return true;
                }}};
        require(!editor.openProject(missingProject) && saveCount == 1 && !persisted.lastProject.has_value() &&
                    persisted.recentProjects.empty(),
                "A missing startup project must be cleared and persisted before showing the navigator.");

        ImGui::NewFrame();
        ImGui::GetMainViewport()->WorkPos = {0.0f, 0.0f};
        ImGui::GetMainViewport()->WorkSize = io.DisplaySize;
        editor.draw();
        ImGui::EndFrame();
        require(ImGui::FindWindowByName("Project Navigator") != nullptr &&
                    ImGui::FindWindowByName("Viewport") == nullptr &&
                    !editor.viewportInteraction().hasRenderableExtent(),
                "Without a project the navigator must replace editor panels and clear viewport interaction.");

        std::string error;
        require(project.create(temporary.path, "VisibleProject", error), error.c_str());
        ImGui::NewFrame();
        ImGui::GetMainViewport()->WorkPos = {0.0f, 0.0f};
        ImGui::GetMainViewport()->WorkSize = io.DisplaySize;
        editor.draw();
        ImGui::EndFrame();
        require(ImGui::FindWindowByName("Scene Hierarchy") != nullptr &&
                    ImGui::FindWindowByName("Viewport") == nullptr &&
                    !editor.viewportInteraction().hasRenderableExtent(),
                "Persisted hidden panels must stay closed while the remaining editor layout is available.");
        ImGui::DestroyContext();
    }

} // namespace

int main() {
    try {
        testEmptyAndStaleSelection();
        testExactInputCapturePolicy();
        testRenderSettingsPanelAdapterPublishesTypedSnapshots();
        testConsoleReturnsValues();
        testLayoutLifecycleTransitions();
        testEditorNeutralDarkTheme();
        testDockLayoutSkipsNonpositiveWorkSizeAndBuildsAfterRestore();
        testConsoleHistoryAndClearScopes();
        testFailedCommandAppearsOnce();
        testUninitializedFrontendHasNoThreadState();
        testSettingsAndSelectionMutation();
        testCameraControlsUseRenderThreadService();
        testLocalLightEditingAndActorLifecycle();
        testProjectNavigatorAndPersistedWindowVisibility();
        std::cout << "Editor PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor FAIL: " << error.what() << '\n';
        return 1;
    }
}

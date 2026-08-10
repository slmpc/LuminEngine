#include "render/editor/Editor.hpp"
#include "render/editor/EditorLayout.hpp"

#include "assets/ObjLoader.hpp"
#include "render/ImGuiContent.hpp"
#include "render/ImGuiLayer.hpp"
#include "render/ImGuiManager.hpp"
#include "render/LevelRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

namespace {

    static_assert(requires(lumin::render::LevelRenderer& renderer, lumin::render::ImGuiContent* content) {
        renderer.beginUiFrame(content);
    });

    class TestActor final : public lumin::scene::Actor {};

    class CountingContent final : public lumin::render::ImGuiContent {
    public:
        void draw() override {
            ++drawCount;
        }

        int drawCount = 0;
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

    lumin::editor::Editor makeEditor(lumin::scene::Level& level, lumin::scene::Camera& camera,
                                     lumin::render::RenderSettings& settings, lumin::scripting::ScriptRuntime& runtime,
                                     lumin::editor::ViewportImageProvider viewportImage = {}) {
        return lumin::editor::Editor{level,
                                     camera,
                                     settings,
                                     runtime,
                                     [] {
                                         return lumin::render::gi::BackendInfo{"SSAO", false, false};
                                     },
                                     std::move(viewportImage)};
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
        bool viewportImageRequested = false;
        auto editor = makeEditor(level, camera, settings, runtime, [&viewportImageRequested] {
            viewportImageRequested = true;
            return lumin::render::ImGuiViewportImage{0x1234U, 640, 360};
        });

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
        const ImGuiWindow* viewportWindow = ImGui::FindWindowByName("Viewport");
        require(viewportWindow != nullptr && viewportWindow->DockId != 0 && viewportImageRequested,
                "Viewport must be an independent dock window backed by the renderer image provider.");
        const auto viewportState = editor.viewportInteraction();
        const std::uint32_t expectedWidth = static_cast<std::uint32_t>(
            std::max(viewportWindow->ContentRegionRect.GetWidth() * io.DisplayFramebufferScale.x, 1.0f));
        const std::uint32_t expectedHeight = static_cast<std::uint32_t>(
            std::max(viewportWindow->ContentRegionRect.GetHeight() * io.DisplayFramebufferScale.y, 1.0f));
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

    void testUninitializedBeginFrameIsSafe() {
        lumin::render::ImGuiLayer layer;
        layer.newFrame();
        require(!layer.initialized(), "A default ImGui layer must remain uninitialized after a no-op newFrame.");

        lumin::render::ImGuiManager manager;
        CountingContent content;
        manager.beginFrame(&content);
        require(content.drawCount == 0, "Uninitialized beginFrame must not draw content outside a valid ImGui frame.");
        require(!manager.framePrepared(), "An uninitialized manager must not prepare a frame for recording.");
        require(!manager.captureState().uiClaimsInput(),
                "An uninitialized manager must expose an empty capture state.");
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
        editor.setGlobalIlluminationEnabled(false);
        editor.setTaaEnabled(false);
        editor.setExposure(2.25f);
        editor.setSunDirection({0.0f, -1.0f, 0.0f});
        require(nearlyEqual(camera.moveSpeed(), 8.0f) && camera.position() == glm::vec3(1.0f, 2.0f, 3.0f),
                "Camera controls must mutate the borrowed camera.");
        require(!settings.directLighting.enabled && !settings.shadows.enabled && !settings.globalIllumination.enabled &&
                    !settings.temporalAa.enabled && nearlyEqual(settings.toneMapping.exposure, 2.25f) &&
                    level.environment().sun.direction == glm::vec3(0.0f, -1.0f, 0.0f) && camera.cutEpoch() == 1,
                "Render/GI controls must mutate feature settings, scene lighting, and explicit camera-cut state.");

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

} // namespace

int main() {
    try {
        testEmptyAndStaleSelection();
        testExactInputCapturePolicy();
        testConsoleReturnsValues();
        testLayoutLifecycleTransitions();
        testDockLayoutSkipsNonpositiveWorkSizeAndBuildsAfterRestore();
        testConsoleHistoryAndClearScopes();
        testFailedCommandAppearsOnce();
        testUninitializedBeginFrameIsSafe();
        testSettingsAndSelectionMutation();
        std::cout << "Editor PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor FAIL: " << error.what() << '\n';
        return 1;
    }
}

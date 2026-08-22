#include "application/Application.hpp"

#include "project/ProjectSession.hpp"
#include "render/LevelRenderer.hpp"
#include "render/editor/Editor.hpp"
#include "render/platform/RenderDocAttachment.hpp"
#include "render/platform/Window.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/CameraController.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

namespace lumin::core {
    namespace {

        std::filesystem::path recentProjectsPath() {
            char* preferencePath = SDL_GetPrefPath("Lumin", "LuminEngine");
            if (preferencePath == nullptr) {
                return {};
            }
            const std::filesystem::path result = std::filesystem::path{preferencePath} / "recent-projects.txt";
            SDL_free(preferencePath);
            return result;
        }

        platform::RenderDocAttachment attachRenderDoc(const ApplicationConfig& config) {
            if (!config.enableRenderDoc) {
                return {};
            }
            if (!config.renderDocPath.has_value()) {
                throw std::invalid_argument("RenderDoc is enabled, but no library path was supplied");
            }

            platform::RenderDocAttachment attachment(*config.renderDocPath);
            std::cout << "RenderDoc attached from '" << config.renderDocPath->string() << "'.\n";
            return attachment;
        }

        class RendererIdleGuard {
        public:
            explicit RendererIdleGuard(render::LevelRenderer& renderer) noexcept : renderer_(renderer) {
            }

            ~RendererIdleGuard() {
                try {
                    renderer_.waitIdle();
                } catch (...) {
                    // Destructors cannot report a second failure while unwinding.
                }
            }

        private:
            render::LevelRenderer& renderer_;
        };

    } // namespace

    struct Application::Impl {
        Impl(ApplicationConfig applicationConfig, std::unique_ptr<game::Game> applicationGame)
            : config(std::move(applicationConfig)), game(std::move(applicationGame)),
              renderDoc(attachRenderDoc(config)),
              window(platform::WindowDesc{config.width, config.height, config.title}),
              vulkan(window, render::VulkanContextDesc{.applicationName = config.title,
                                                       .enableValidation =
#if defined(LUMIN_ENABLE_VALIDATION)
                                                           true,
#else
                                                           false,
#endif
                                                       .rayTracing = {}}),
              scripts(scripting::ScriptRuntimeOptions{.scriptRoot = config.scriptRoot,
                                                      .diagnosticCapacity = 256,
                                                      .consoleHistoryCapacity = 128,
                                                      .diagnosticSink = {}}),
              project(level, camera, scripts) {
            if (!game) {
                throw std::invalid_argument("Application requires a Game instance");
            }
        }

        int run() {
            game::GameContext context{level, camera, scripts};
            startupScript = game::initializeGame(*game, context, config.startupScript);

            const std::filesystem::path shaderDirectory =
#if defined(LUMIN_SHADER_DIR)
                LUMIN_SHADER_DIR;
#else
                "shaders";
#endif
            renderer = std::make_unique<render::LevelRenderer>(window, vulkan, level, shaderDirectory);
            RendererIdleGuard idleGuard{*renderer};
            editor = std::make_unique<editor::Editor>(
                level, camera, renderSettings, scripts,
                [this] {
                    return renderer->globalIlluminationBackendInfo();
                },
                [this] {
                    return renderer->viewportImage();
                },
                &project,
                editor::EditorDialogServices{.openProject =
                                                 [this](editor::DialogResultCallback callback) {
                                                     window.showOpenFileDialog({{"Lumin Project", "luminproject"}},
                                                                               false, std::move(callback));
                                                 },
                                             .openFolder =
                                                 [this](editor::DialogResultCallback callback) {
                                                     window.showOpenFolderDialog(std::move(callback));
                                                 },
                                             .loadRecentProjects =
                                                 [] {
                                                     std::vector<std::filesystem::path> result;
                                                     std::ifstream stream(recentProjectsPath());
                                                     std::string line;
                                                     while (std::getline(stream, line) && result.size() < 10) {
                                                         if (!line.empty() && std::filesystem::exists(line)) {
                                                             result.emplace_back(line);
                                                         }
                                                     }
                                                     return result;
                                                 },
                                             .saveRecentProjects =
                                                 [](const std::vector<std::filesystem::path>& projects) {
                                                     const std::filesystem::path path = recentProjectsPath();
                                                     if (path.empty()) {
                                                         return;
                                                     }
                                                     std::filesystem::create_directories(path.parent_path());
                                                     std::ofstream stream(path, std::ios::trunc);
                                                     for (const auto& projectPath : projects) {
                                                         stream << projectPath.generic_string() << '\n';
                                                     }
                                                 }});

            std::cout << "Level renderer ready: models=" << renderer->modelCount()
                      << " mdiDraws=" << renderer->mdiDrawCount()
                      << " gbuffer=position+normalRoughness+albedoMetallic+motion csm=4 ssao=on taa=on\n";

            auto previousTime = std::chrono::steady_clock::now();
            while (!window.shouldClose()) {
                window.pollEvents();
                if (window.shouldClose()) {
                    if (project.dirty()) {
                        window.cancelCloseRequest();
                        editor->requestExit();
                    } else {
                        break;
                    }
                }
                renderer->beginUiFrame(editor.get());
                if (editor->exitRequested()) {
                    renderer->cancelUiFrame();
                    break;
                }
                const editor::ViewportInteractionState viewport = editor->viewportInteraction();
                if (viewport.hasRenderableExtent()) {
                    renderer->requestViewportExtent(viewport.width, viewport.height);
                }
                const bool middleMouseDown = window.isMouseButtonDown(platform::MouseButton::Middle);
                if (!viewportLookActive && middleMouseDown && viewport.hovered) {
                    window.setRelativeMouseMode(true);
                    viewportLookActive = true;
                } else if (viewportLookActive && !middleMouseDown) {
                    window.setRelativeMouseMode(false);
                    viewportLookActive = false;
                }
                const render::ImGuiCaptureState capture = renderer->imguiCaptureState();
                const bool escapeDown = window.isKeyDown(platform::Key::Escape);
                const game::InputRoutingDecision routing =
                    game::routeInput(!viewportLookActive && capture.uiClaimsInput(), escapeDown);
                if (routing.exitOnEscape && !escapeHeld) {
                    editor->requestExit();
                }
                escapeHeld = escapeDown;
                if (editor->exitRequested()) {
                    renderer->cancelUiFrame();
                    break;
                }

                const auto currentTime = std::chrono::steady_clock::now();
                const float deltaSeconds =
                    std::clamp(std::chrono::duration<float>(currentTime - previousTime).count(), 0.0f, 0.1f);
                previousTime = currentTime;

                game::GameInput input;
                if (routing.dispatchGameInput || routing.updateCamera) {
                    input.forward = static_cast<float>(window.isKeyDown(platform::Key::W)) -
                                    static_cast<float>(window.isKeyDown(platform::Key::S));
                    input.right = static_cast<float>(window.isKeyDown(platform::Key::D)) -
                                  static_cast<float>(window.isKeyDown(platform::Key::A));
                    input.up = static_cast<float>(window.isKeyDown(platform::Key::Space)) -
                               static_cast<float>(window.isKeyDown(platform::Key::LeftControl));
                }
                if (routing.updateCamera) {
                    const platform::MouseDelta mouseDelta =
                        viewportLookActive ? window.mouseDelta() : platform::MouseDelta{};
                    scene::CameraController::update(camera,
                                                    scene::CameraInput{.forward = input.forward,
                                                                       .right = input.right,
                                                                       .up = input.up,
                                                                       .lookDeltaX = mouseDelta.x,
                                                                       .lookDeltaY = mouseDelta.y},
                                                    deltaSeconds);
                }

                game::advanceGameFrame(*game, context,
                                       routing.dispatchGameInput ? std::optional<game::GameInput>{input} : std::nullopt,
                                       deltaSeconds);
                renderer->drawFrame(camera, renderSettings, editor.get());
            }
            return 0;
        }

        ApplicationConfig config;
        std::unique_ptr<game::Game> game;
        platform::RenderDocAttachment renderDoc;
        platform::Window window;
        render::VulkanContext vulkan;
        scene::Level level;
        scene::Camera camera;
        scripting::ScriptRuntime scripts;
        render::RenderSettings renderSettings;
        project::ProjectSession project;
        std::unique_ptr<render::LevelRenderer> renderer;
        std::unique_ptr<editor::Editor> editor;
        std::optional<scripting::ScriptHandle> startupScript;
        bool viewportLookActive = false;
        bool escapeHeld = false;
    };

    Application::Application(ApplicationConfig config, std::unique_ptr<game::Game> game)
        : impl_(std::make_unique<Impl>(std::move(config), std::move(game))) {
    }

    Application::~Application() = default;

    int Application::run() {
        return impl_->run();
    }

} // namespace lumin::core

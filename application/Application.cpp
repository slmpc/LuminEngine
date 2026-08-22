#include "application/Application.hpp"

#include "config/EngineSettings.hpp"
#include "project/ProjectSession.hpp"
#include "render/LevelRenderer.hpp"
#include "render/editor/Editor.hpp"
#include "render/editor/ImGuiFrontend.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"
#include "render/platform/RenderDocAttachment.hpp"
#include "render/platform/Window.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/CameraController.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

namespace lumin::core {
    namespace {

        std::filesystem::path preferenceFilePath(std::string_view filename) {
            char* preferencePath = SDL_GetPrefPath("Lumin", "LuminEngine");
            if (preferencePath == nullptr) {
                return {};
            }
            const std::filesystem::path result = std::filesystem::path{preferencePath} / filename;
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
            ui = std::make_unique<render::ImGuiFrontend>();
            ui->initialize(window);
            renderer = std::make_unique<render::LevelRenderer>(
                vulkan, render::world::RenderWorldExtractor::extract(level), shaderDirectory, ui->fontAtlas());
            viewportExtent = render::core::RenderExtent{vulkan.swapchainWidth(), vulkan.swapchainHeight()};
            RendererIdleGuard idleGuard{*renderer};
            const std::filesystem::path engineSettingsPath = preferenceFilePath("engine-settings.json");
            const config::EngineSettingsLoadResult loadedSettings =
                config::loadEngineSettings(engineSettingsPath, preferenceFilePath("recent-projects.txt"));
            if (!loadedSettings.diagnostic.empty()) {
                std::cerr << loadedSettings.diagnostic << '\n';
            }
            if (loadedSettings.needsSave) {
                std::string migrationError;
                if (!config::saveEngineSettings(engineSettingsPath, loadedSettings.settings, migrationError)) {
                    std::cerr << migrationError << '\n';
                }
            }
            std::optional<std::filesystem::path> startupProject;
            if (loadedSettings.settings.startupDestination == config::StartupDestination::LastProject) {
                startupProject = loadedSettings.settings.lastProject;
            }
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
                                                 }},
                editor::EditorSettingsServices{
                    .settings = loadedSettings.settings,
                    .save = [engineSettingsPath](const config::EngineSettings& settings, std::string& error) {
                        return config::saveEngineSettings(engineSettingsPath, settings, error);
                    }});
            if (startupProject.has_value()) {
                static_cast<void>(editor->openProject(*startupProject));
            }

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
                ui->beginFrame(editor.get());
                if (editor->exitRequested()) {
                    ui->cancelFrame();
                    break;
                }
                const editor::ViewportInteractionState viewport = editor->viewportInteraction();
                if (viewport.hasRenderableExtent()) {
                    viewportExtent = render::core::RenderExtent{viewport.width, viewport.height};
                }
                const bool middleMouseDown = window.isMouseButtonDown(platform::MouseButton::Middle);
                if (!project.hasProject() && viewportLookActive) {
                    window.setRelativeMouseMode(false);
                    viewportLookActive = false;
                }
                if (!viewportLookActive && middleMouseDown && viewport.hovered) {
                    window.setRelativeMouseMode(true);
                    viewportLookActive = true;
                } else if (viewportLookActive && !middleMouseDown) {
                    window.setRelativeMouseMode(false);
                    viewportLookActive = false;
                }
                const render::ImGuiCaptureState capture = ui->captureState();
                const bool escapeDown = window.isKeyDown(platform::Key::Escape);
                const game::InputRoutingDecision routing =
                    project.hasProject() ? game::routeInput(!viewportLookActive && capture.uiClaimsInput(), escapeDown)
                                         : game::InputRoutingDecision{};
                if (routing.exitOnEscape && !escapeHeld) {
                    editor->requestExit();
                }
                escapeHeld = escapeDown;
                if (editor->exitRequested()) {
                    ui->cancelFrame();
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
                const VkExtent2D framebufferExtent = window.framebufferExtent();
                renderer->drawFrame(framePacketBuilder.build(
                    level, camera, render::pipelines::makeDefaultRenderSettingsSnapshot(renderSettings),
                    ui->finishFrame(),
                    render::core::SurfaceState{
                        .windowExtent = {framebufferExtent.width, framebufferExtent.height},
                        .viewportExtent = viewportExtent,
                        .framebufferResized = window.framebufferResized(),
                        .minimized = framebufferExtent.width == 0 || framebufferExtent.height == 0,
                    }));
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
        render::core::RenderFramePacketBuilder framePacketBuilder;
        render::core::RenderExtent viewportExtent{1280, 720};
        project::ProjectSession project;
        std::unique_ptr<render::ImGuiFrontend> ui;
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

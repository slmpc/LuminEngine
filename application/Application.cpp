#include "application/Application.hpp"

#include "application/LogicRuntime.hpp"
#include "config/EngineSettings.hpp"
#include "render/Renderer.hpp"
#include "render/editor/Editor.hpp"
#include "render/editor/ImGuiFrontend.hpp"
#include "render/editor/RenderSettingsPanelAdapter.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"
#include "render/platform/RenderDocAttachment.hpp"
#include "render/platform/Window.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"

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

    } // namespace

    struct Application::Impl {
        Impl(ApplicationConfig applicationConfig, std::unique_ptr<game::Game> applicationGame)
            : config(std::move(applicationConfig)), pendingGame(std::move(applicationGame)),
              renderDoc(attachRenderDoc(config)),
              window(platform::WindowDesc{config.width, config.height, config.title}) {
            if (!pendingGame) {
                throw std::invalid_argument("Application requires a Game instance");
            }
        }

        int run() {
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
            logic = std::make_unique<LogicRuntime>(LogicRuntimeConfig{.scriptRoot = config.scriptRoot,
                                                                      .startupScript = config.startupScript,
                                                                      .startupProject = std::move(startupProject)},
                                                   std::move(pendingGame));
            frameLogicSnapshot = logic->snapshot();
            if (frameLogicSnapshot == nullptr || frameLogicSnapshot->renderWorld == nullptr) {
                throw std::runtime_error("Logic Runtime did not publish an initial world snapshot.");
            }

            const std::filesystem::path shaderDirectory =
#if defined(LUMIN_SHADER_DIR)
                LUMIN_SHADER_DIR;
#else
                "shaders";
#endif
            ui = std::make_unique<render::ImGuiFrontend>();
            ui->initialize(window);
            const VkExtent2D initialExtent = window.framebufferExtent();
            auto vulkanBootstrap = std::make_unique<render::VulkanSurfaceBootstrap>(
                window, render::VulkanContextDesc{.applicationName = config.title,
                                                  .enableValidation =
#if defined(LUMIN_ENABLE_VALIDATION)
                                                      true,
#else
                                                      false,
#endif
                                                  .rayTracing = {}});
            viewportExtent = render::core::RenderExtent{initialExtent.width, initialExtent.height};
            renderer = std::make_unique<render::Renderer>(std::move(vulkanBootstrap), frameLogicSnapshot->renderWorld,
                                                          shaderDirectory, ui->fontAtlas(),
                                                          render::pipelines::makeDefaultRenderPipelineSessionFactory());
            editor = std::make_unique<editor::Editor>(
                editor::EditorLogicServices{.snapshot =
                                                [this] {
                                                    return frameLogicSnapshot;
                                                },
                                            .submit =
                                                [this](editor::EditorLogicCommand command) {
                                                    return logic->submit(std::move(command));
                                                },
                                            .drainResults =
                                                [this] {
                                                    return logic->drainResults();
                                                }},
                renderSettingsAdapter.editable(),
                [this] {
                    rendererStatusCache = renderer->status();
                    return render::gi::BackendInfo{rendererStatusCache.globalIlluminationBackend,
                                                   rendererStatusCache.globalIlluminationTemporal,
                                                   rendererStatusCache.hardwareRayTracing};
                },
                [this] {
                    rendererStatusCache = renderer->status();
                    return render::ImGuiViewportImage{rendererStatusCache.viewport.textureId,
                                                      rendererStatusCache.viewport.width,
                                                      rendererStatusCache.viewport.height};
                },
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
                    .save =
                        [engineSettingsPath](const config::EngineSettings& settings, std::string& error) {
                            return config::saveEngineSettings(engineSettingsPath, settings, error);
                        }},
                [this] {
                    rendererStatusCache = renderer->status();
                    return rendererStatusCache.presentedFramesPerSecond;
                });

            rendererStatusCache = renderer->status();
            std::cout << "Renderer ready: models=" << rendererStatusCache.modelCount
                      << " mdiDraws=" << rendererStatusCache.mdiDrawCount
                      << " gbuffer=position+normalRoughness+albedoMetallic+motion csm=4 ssao=on taa=on\n";

            while (!window.shouldClose()) {
                logic->rethrowIfFailed();
                if (std::shared_ptr<const editor::EditorLogicSnapshot> latest = logic->snapshot(); latest != nullptr) {
                    frameLogicSnapshot = std::move(latest);
                }

                window.pollEvents();
                if (window.shouldClose()) {
                    if (frameLogicSnapshot->project.dirty) {
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
                if (!frameLogicSnapshot->project.open && viewportLookActive) {
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
                    frameLogicSnapshot->project.open
                        ? game::routeInput(!viewportLookActive && capture.uiClaimsInput(), escapeDown)
                        : game::InputRoutingDecision{};
                if (routing.exitOnEscape && !escapeHeld) {
                    editor->requestExit();
                }
                escapeHeld = escapeDown;
                if (editor->exitRequested()) {
                    ui->cancelFrame();
                    break;
                }

                game::GameInput gameInput;
                if (routing.dispatchGameInput || routing.updateCamera) {
                    gameInput.forward = static_cast<float>(window.isKeyDown(platform::Key::W)) -
                                        static_cast<float>(window.isKeyDown(platform::Key::S));
                    gameInput.right = static_cast<float>(window.isKeyDown(platform::Key::D)) -
                                      static_cast<float>(window.isKeyDown(platform::Key::A));
                    gameInput.up = static_cast<float>(window.isKeyDown(platform::Key::Space)) -
                                   static_cast<float>(window.isKeyDown(platform::Key::LeftControl));
                }
                const platform::MouseDelta mouseDelta =
                    routing.updateCamera && viewportLookActive ? window.mouseDelta() : platform::MouseDelta{};
                logic->publishInput(LogicInputState{
                    .game = routing.dispatchGameInput ? std::optional<game::GameInput>{gameInput} : std::nullopt,
                    .camera = routing.updateCamera ? std::optional<scene::CameraInput>{{.forward = gameInput.forward,
                                                                                        .right = gameInput.right,
                                                                                        .up = gameInput.up,
                                                                                        .lookDeltaX = mouseDelta.x,
                                                                                        .lookDeltaY = mouseDelta.y}}
                                                   : std::nullopt});

                const VkExtent2D framebufferExtent = window.framebufferExtent();
                const bool framebufferResized = window.framebufferResized();
                if (framebufferResized) {
                    ++surfaceRevision;
                    window.resetFramebufferResized();
                }
                const ImDrawData* drawData = ui->finishFrame();
                if (drawData == nullptr) {
                    throw std::runtime_error("Dear ImGui did not produce draw data for the current frame.");
                }
                static_cast<void>(renderer->drawFrame(
                    framePacketBuilder.build(
                        frameLogicSnapshot->renderWorld, frameLogicSnapshot->camera, renderSettingsAdapter.snapshot(),
                        render::core::SurfaceState{
                            .windowExtent = {framebufferExtent.width, framebufferExtent.height},
                            .viewportExtent = viewportExtent,
                            .framebufferResized = framebufferResized,
                            .surfaceRevision = surfaceRevision,
                            .minimized = framebufferExtent.width == 0 || framebufferExtent.height == 0,
                        }),
                    *drawData));
            }

            logic->stop();
            renderer->waitIdle();
            renderer->shutdown();
            editor.reset();
            ui.reset();
            return 0;
        }

        ApplicationConfig config;
        std::unique_ptr<game::Game> pendingGame;
        platform::RenderDocAttachment renderDoc;
        platform::Window window;
        render::editor::RenderSettingsPanelAdapter renderSettingsAdapter;
        render::core::RenderFramePacketBuilder framePacketBuilder;
        render::core::RenderExtent viewportExtent{1280, 720};
        std::unique_ptr<LogicRuntime> logic;
        std::shared_ptr<const editor::EditorLogicSnapshot> frameLogicSnapshot;
        std::unique_ptr<render::ImGuiFrontend> ui;
        std::unique_ptr<render::Renderer> renderer;
        render::RendererStatusSnapshot rendererStatusCache;
        std::unique_ptr<editor::Editor> editor;
        bool viewportLookActive = false;
        bool escapeHeld = false;
        std::uint64_t surfaceRevision = 0;
    };

    Application::Application(ApplicationConfig config, std::unique_ptr<game::Game> game)
        : impl_(std::make_unique<Impl>(std::move(config), std::move(game))) {
    }

    Application::~Application() = default;

    int Application::run() {
        return impl_->run();
    }

} // namespace lumin::core

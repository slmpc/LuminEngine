#include "lumin/core/Application.hpp"

#include "lumin/editor/Editor.hpp"
#include "lumin/platform/RenderDocAttachment.hpp"
#include "lumin/platform/Window.hpp"
#include "lumin/render/LevelRenderer.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/scene/CameraController.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace lumin::core {
    namespace {

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
              vulkan(window, render::VulkanContextDesc{config.title,
#if defined(LUMIN_ENABLE_VALIDATION)
                                                       true
#else
                                                       false
#endif
                             }),
              scripts(scripting::ScriptRuntimeOptions{.scriptRoot = config.scriptRoot,
                                                      .diagnosticCapacity = 256,
                                                      .consoleHistoryCapacity = 128,
                                                      .diagnosticSink = {}}) {
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
            editor = std::make_unique<editor::Editor>(level, camera, renderSettings, scripts, [this] {
                return renderer->globalIlluminationBackendInfo();
            });

            std::cout << "Level renderer ready: models=" << renderer->modelCount()
                      << " mdiDraws=" << renderer->mdiDrawCount()
                      << " gbuffer=position+normalRoughness+albedoMetallic+motion csm=4 ssao=on taa=on\n";

            auto previousTime = std::chrono::steady_clock::now();
            while (!window.shouldClose()) {
                window.pollEvents();
                renderer->beginUiFrame(editor.get());
                const render::ImGuiCaptureState capture = renderer->imguiCaptureState();
                const game::InputRoutingDecision routing =
                    game::routeInput(capture.uiClaimsInput(), window.isKeyDown(platform::Key::Escape));
                if (routing.exitOnEscape) {
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
                    scene::CameraController::update(
                        camera, scene::CameraInput{.forward = input.forward, .right = input.right, .up = input.up},
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
        std::unique_ptr<render::LevelRenderer> renderer;
        std::unique_ptr<editor::Editor> editor;
        std::optional<scripting::ScriptHandle> startupScript;
    };

    Application::Application(ApplicationConfig config, std::unique_ptr<game::Game> game)
        : impl_(std::make_unique<Impl>(std::move(config), std::move(game))) {
    }

    Application::~Application() = default;

    int Application::run() {
        return impl_->run();
    }

} // namespace lumin::core

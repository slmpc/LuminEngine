#include "game/Game.hpp"

#include "core/Application.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

    void require(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string{message});
        }
    }

    class TemporaryScripts {
    public:
        TemporaryScripts() {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path() / ("lumin-game-engine-tests-" + std::to_string(suffix));
            std::filesystem::create_directories(root_);
        }

        ~TemporaryScripts() {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }

        void write(std::string_view name, std::string_view source) const {
            std::ofstream stream(root_ / name, std::ios::binary | std::ios::trunc);
            require(stream.is_open(), "Test script must open for writing.");
            stream << source;
            require(stream.good(), "Test script must be written completely.");
        }

        [[nodiscard]] const std::filesystem::path& root() const noexcept {
            return root_;
        }

    private:
        std::filesystem::path root_;
    };

    class RecordingGame final : public lumin::game::Game {
    public:
        void initialize(lumin::game::GameContext& context) override {
            ++initializeCount;
            context.camera.setPosition({1.0f, 2.0f, 3.0f});
        }

        void tick(lumin::game::GameContext&, float deltaSeconds) override {
            ++tickCount;
            accumulatedSeconds += deltaSeconds;
        }

        void handleInput(lumin::game::GameContext&, const lumin::game::GameInput& input) override {
            ++inputCount;
            lastInput = input;
        }

        int initializeCount = 0;
        int inputCount = 0;
        int tickCount = 0;
        float accumulatedSeconds = 0.0f;
        lumin::game::GameInput lastInput;
    };

    void testGameLifecycleWithoutVulkan() {
        TemporaryScripts scripts;
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime runtime({.scriptRoot = scripts.root()});
        lumin::game::GameContext context{level, camera, runtime};
        RecordingGame game;

        (void)lumin::game::initializeGame(game, context, std::nullopt);
        game.handleInput(context, {.forward = 1.0f});
        lumin::game::tickGame(game, context, 0.25f);

        require(game.initializeCount == 1, "Game initialize must run exactly once.");
        require(game.inputCount == 1 && game.lastInput.forward == 1.0f, "Game input must use platform-free data.");
        require(game.tickCount == 1 && game.accumulatedSeconds == 0.25f, "Game tick must receive delta time.");
        require(camera.position() == glm::vec3(1.0f, 2.0f, 3.0f), "Game must receive the engine camera.");
    }

    void testApplicationConfigDefaults() {
        const lumin::core::ApplicationConfig config;
        require(config.width == 1280 && config.height == 720, "Application default size must remain 1280x720.");
        require(config.title == "Lumin Engine", "Application title must remain Lumin Engine.");
        require(sizeof(lumin::core::Application) == sizeof(void*), "Application public ownership must remain PImpl.");
    }

    void testStartupScriptSuccess() {
        TemporaryScripts scripts;
        scripts.write("startup.lua", R"lua(
return {
    on_spawn = function(actor, level)
        actor:translate(1.0, 2.0, 3.0)
    end
}
)lua");
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime runtime({.scriptRoot = scripts.root()});
        lumin::game::GameContext context{level, camera, runtime};
        RecordingGame game;

        const auto startup = lumin::game::initializeGame(game, context, std::filesystem::path{"startup.lua"});

        require(startup.has_value() && startup->isValid(), "A valid startup script must return its handle.");
        require(level.actorCount() == 1, "A valid startup script must own one actor.");
    }

    void testStartupScriptErrorsDoNotLeakActors() {
        TemporaryScripts scripts;
        scripts.write("malformed.lua", "return { on_tick = function( }");
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime runtime({.scriptRoot = scripts.root()});
        lumin::game::GameContext context{level, camera, runtime};

        for (const auto& source : {std::filesystem::path{"malformed.lua"}, std::filesystem::path{"missing.lua"}}) {
            RecordingGame game;
            bool threw = false;
            try {
                (void)lumin::game::initializeGame(game, context, source);
            } catch (const std::runtime_error& error) {
                threw = std::string_view{error.what()}.find(source.string()) != std::string_view::npos;
            }
            require(threw, "Invalid startup script errors must name the requested source.");
            require(level.actorCount() == 0, "Invalid startup scripts must not leak actors.");
        }
    }

    void testInputRouting() {
        const auto gameplay = lumin::game::routeInput(false, true);
        require(gameplay.dispatchGameInput && gameplay.updateCamera && gameplay.exitOnEscape,
                "Uncaptured input must reach gameplay, camera, and Escape handling.");

        const auto editor = lumin::game::routeInput(true, true);
        require(!editor.dispatchGameInput && !editor.updateCamera && !editor.exitOnEscape,
                "Editor capture must suppress all gameplay input routing.");
    }

    void testCapturedInputStillAdvancesSimulation() {
        TemporaryScripts scripts;
        scripts.write("tick.lua", R"lua(
return {
    on_tick = function(actor, level, delta_seconds)
        actor:translate(delta_seconds, 0.0, 0.0)
    end
}
)lua");
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime runtime({.scriptRoot = scripts.root()});
        lumin::game::GameContext context{level, camera, runtime};
        RecordingGame game;
        const auto startup = lumin::game::initializeGame(game, context, std::filesystem::path{"tick.lua"});

        const auto routing = lumin::game::routeInput(true, false);
        lumin::game::advanceGameFrame(
            game, context,
            routing.dispatchGameInput ? std::optional<lumin::game::GameInput>{lumin::game::GameInput{.forward = 1.0f}}
                                      : std::nullopt,
            0.25f);

        const auto info = runtime.script(*startup);
        const auto* actor = info.has_value() ? level.actor(info->actor) : nullptr;
        require(game.inputCount == 0, "Captured UI input must not dispatch to Game.");
        require(game.tickCount == 1, "Captured UI input must not suppress Game tick.");
        require(actor != nullptr && actor->transform().position.x == 0.25f,
                "Captured UI input must not suppress Level or Lua tick.");
    }

} // namespace

int main() {
    testApplicationConfigDefaults();
    testGameLifecycleWithoutVulkan();
    testStartupScriptSuccess();
    testStartupScriptErrorsDoNotLeakActors();
    testInputRouting();
    testCapturedInputStillAdvancesSimulation();
    return 0;
}

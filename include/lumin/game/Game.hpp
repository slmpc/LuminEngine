#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"
#include "lumin/scripting/ScriptRuntime.hpp"

namespace lumin::game {

    struct GameContext {
        scene::Level& level;
        scene::Camera& camera;
        scripting::ScriptRuntime& scripts;
    };

    struct GameInput {
        float forward = 0.0f;
        float right = 0.0f;
        float up = 0.0f;
    };

    class Game {
    public:
        virtual ~Game() = default;

        virtual void initialize(GameContext& context) = 0;
        virtual void handleInput(GameContext&, const GameInput&) {
        }
        virtual void tick(GameContext& context, float deltaSeconds) = 0;
    };

    struct InputRoutingDecision {
        bool dispatchGameInput = false;
        bool updateCamera = false;
        bool exitOnEscape = false;
    };

    [[nodiscard]] inline InputRoutingDecision routeInput(bool uiClaimsInput, bool escapeDown) noexcept {
        if (uiClaimsInput) {
            return {};
        }
        return {.dispatchGameInput = true, .updateCamera = true, .exitOnEscape = escapeDown};
    }

    [[nodiscard]] inline std::optional<scripting::ScriptHandle>
    initializeGame(Game& game, GameContext& context, const std::optional<std::filesystem::path>& startupScript) {
        game.initialize(context);
        if (!startupScript.has_value()) {
            return std::nullopt;
        }

        const scripting::ScriptSpawnResult result = context.scripts.spawn(context.level, *startupScript);
        if (!result) {
            std::string message = "Failed to load startup script '" + startupScript->string() + "'";
            if (result.result.error.has_value() && !result.result.error->message.empty()) {
                message += ": " + result.result.error->message;
            }
            throw std::runtime_error(message);
        }
        return result.script;
    }

    inline void tickGame(Game& game, GameContext& context, float deltaSeconds) {
        game.tick(context, deltaSeconds);
    }

    inline void advanceGameFrame(Game& game, GameContext& context, const std::optional<GameInput>& input,
                                 float deltaSeconds) {
        if (input.has_value()) {
            game.handleInput(context, *input);
        }
        tickGame(game, context, deltaSeconds);
        context.level.tick(deltaSeconds);
    }

} // namespace lumin::game

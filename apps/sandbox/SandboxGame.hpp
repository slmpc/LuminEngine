#pragma once

#include <filesystem>
#include <optional>

#include "lumin/game/Game.hpp"

namespace lumin::sandbox {

    struct SandboxGameConfig {
        std::optional<std::filesystem::path> objPath;
    };

    class SandboxGame final : public game::Game {
    public:
        explicit SandboxGame(SandboxGameConfig config = {});

        void initialize(game::GameContext& context) override;
        void tick(game::GameContext& context, float deltaSeconds) override;

    private:
        SandboxGameConfig config_;
    };

} // namespace lumin::sandbox

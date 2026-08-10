#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "game/Game.hpp"

namespace lumin::core {

    struct ApplicationConfig {
        int width = 1280;
        int height = 720;
        std::string title = "Lumin Engine";
        std::filesystem::path scriptRoot;
        std::optional<std::filesystem::path> startupScript;
        bool enableRenderDoc = false;
        std::optional<std::filesystem::path> renderDocPath;
    };

    class Application {
    public:
        Application(ApplicationConfig config, std::unique_ptr<game::Game> game);
        ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        int run();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::core

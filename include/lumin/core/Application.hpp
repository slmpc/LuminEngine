#pragma once

#include <filesystem>
#include <optional>
#include <memory>
#include <string>

#include "lumin/assets/ObjLoader.hpp"
#include "lumin/platform/Window.hpp"
#include "lumin/render/LevelRenderer.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"

namespace lumin::core {

    struct ApplicationConfig {
        int width = 1280;
        int height = 720;
        std::string title = "Lumin Engine";
        std::optional<std::filesystem::path> objPath;
    };

    class Application {
    public:
        explicit Application(ApplicationConfig config);
        ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        int run();

    private:
        void loadScene();

        ApplicationConfig config_;
        platform::Window window_;
        render::VulkanContext vulkan_;
        scene::Level level_;
        scene::Camera camera_;
        std::unique_ptr<render::LevelRenderer> renderer_;
        render::RenderSettings renderSettings_;
    };

} // namespace lumin::core

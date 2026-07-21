#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "lumin/assets/ObjLoader.hpp"
#include "lumin/platform/Window.hpp"
#include "lumin/render/FrameGraph.hpp"
#include "lumin/render/ObjRenderer.hpp"
#include "lumin/render/VulkanContext.hpp"

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
        void buildFrameGraph();

        ApplicationConfig config_;
        platform::Window window_;
        render::VulkanContext vulkan_;
        render::FrameGraph frameGraph_;
        std::optional<assets::Mesh> mesh_;
        render::RenderSettings renderSettings_;
    };

} // namespace lumin::core

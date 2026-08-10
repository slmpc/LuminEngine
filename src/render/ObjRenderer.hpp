#pragma once

#include <filesystem>
#include <memory>

#include "assets/ObjLoader.hpp"
#include "render/RenderSettings.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::render {
    class VulkanContext;

} // namespace lumin::render

namespace lumin::scene {
    class Camera;
}

namespace lumin::render {

    class ObjRenderer {
    public:
        ObjRenderer(platform::Window& window, VulkanContext& context, const assets::Mesh& mesh,
                    std::filesystem::path shaderDirectory);
        ~ObjRenderer();

        ObjRenderer(const ObjRenderer&) = delete;
        ObjRenderer& operator=(const ObjRenderer&) = delete;

        void drawFrame(scene::Camera& camera, RenderSettings& settings);
        void waitIdle() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}

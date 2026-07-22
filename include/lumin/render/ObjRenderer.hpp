#pragma once

#include <filesystem>
#include <memory>

#include "lumin/assets/ObjLoader.hpp"
#include "lumin/render/RenderSettings.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::render {
    class VulkanContext;

    class ObjRenderer {
    public:
        ObjRenderer(platform::Window& window, VulkanContext& context, const assets::Mesh& mesh,
                    std::filesystem::path shaderDirectory);
        ~ObjRenderer();

        ObjRenderer(const ObjRenderer&) = delete;
        ObjRenderer& operator=(const ObjRenderer&) = delete;

        void drawFrame(RenderSettings& settings);
        void waitIdle() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}

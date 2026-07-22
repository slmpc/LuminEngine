#include "lumin/render/ObjRenderer.hpp"

#include "lumin/render/LevelRenderer.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"

#include <stdexcept>
#include <utility>

namespace lumin::render {

    struct ObjRenderer::Impl {
        scene::Level level;
        scene::Camera camera;
        std::unique_ptr<LevelRenderer> renderer;

        Impl(platform::Window& window, VulkanContext& context, const assets::Mesh& mesh,
             std::filesystem::path shaderDirectory) {
            if (mesh.empty()) {
                throw std::invalid_argument("ObjRenderer requires a non-empty mesh.");
            }
            const scene::MeshHandle handle = level.addMesh(mesh);
            level.addModel(handle);
            renderer = std::make_unique<LevelRenderer>(window, context, level, std::move(shaderDirectory));
        }
    };

    ObjRenderer::ObjRenderer(platform::Window& window, VulkanContext& context, const assets::Mesh& mesh,
                             std::filesystem::path shaderDirectory)
        : impl_(std::make_unique<Impl>(window, context, mesh, std::move(shaderDirectory))) {
    }

    ObjRenderer::~ObjRenderer() = default;

    void ObjRenderer::drawFrame(RenderSettings& settings) {
        impl_->camera.setPosition(settings.cameraPosition);
        impl_->renderer->drawFrame(impl_->camera, settings);
    }

    void ObjRenderer::waitIdle() const {
        impl_->renderer->waitIdle();
    }

}

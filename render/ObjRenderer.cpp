#include "render/ObjRenderer.hpp"

#include "render/LevelRenderer.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/Camera.hpp"
#include "scene/Level.hpp"

#include <stdexcept>
#include <utility>

namespace lumin::render {

    struct ObjRenderer::Impl {
        scene::Level level;
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

    void ObjRenderer::drawFrame(scene::Camera& camera, RenderSettings& settings) {
        impl_->renderer->drawFrame(camera, settings);
    }

    void ObjRenderer::waitIdle() const {
        impl_->renderer->waitIdle();
    }

}

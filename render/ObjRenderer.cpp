#include "render/ObjRenderer.hpp"

#include "render/LevelRenderer.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/Camera.hpp"
#include "scene/Level.hpp"

#include <stdexcept>
#include <utility>

namespace lumin::render {

    namespace {

        core::UiFontAtlas fallbackFontAtlas() {
            return core::UiFontAtlas{.width = 1, .height = 1, .rgba8 = {255, 255, 255, 255}};
        }

    } // namespace

    struct ObjRenderer::Impl {
        scene::Level level;
        std::unique_ptr<LevelRenderer> renderer;

        Impl(VulkanContext& context, const assets::Mesh& mesh, std::filesystem::path shaderDirectory) {
            if (mesh.empty()) {
                throw std::invalid_argument("ObjRenderer requires a non-empty mesh.");
            }
            const scene::MeshHandle handle = level.addMesh(mesh);
            level.addModel(handle);
            renderer = std::make_unique<LevelRenderer>(context, level, std::move(shaderDirectory), fallbackFontAtlas());
        }
    };

    ObjRenderer::ObjRenderer(VulkanContext& context, const assets::Mesh& mesh, std::filesystem::path shaderDirectory)
        : impl_(std::make_unique<Impl>(context, mesh, std::move(shaderDirectory))) {
    }

    ObjRenderer::~ObjRenderer() = default;

    void ObjRenderer::drawFrame(scene::Camera& camera, RenderSettings& settings) {
        impl_->renderer->drawFrame(camera, settings);
    }

    void ObjRenderer::waitIdle() const {
        impl_->renderer->waitIdle();
    }

} // namespace lumin::render

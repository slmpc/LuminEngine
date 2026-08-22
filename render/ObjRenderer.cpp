#include "render/ObjRenderer.hpp"

#include "render/LevelRenderer.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"
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
        VulkanContext& context;
        scene::Level level;
        core::RenderFramePacketBuilder framePacketBuilder;
        std::unique_ptr<LevelRenderer> renderer;

        Impl(VulkanContext& contextValue, const assets::Mesh& mesh, std::filesystem::path shaderDirectory)
            : context(contextValue) {
            if (mesh.empty()) {
                throw std::invalid_argument("ObjRenderer requires a non-empty mesh.");
            }
            const scene::MeshHandle handle = level.addMesh(mesh);
            level.addModel(handle);
            renderer = std::make_unique<LevelRenderer>(contextValue, world::RenderWorldExtractor::extract(level),
                                                       std::move(shaderDirectory), fallbackFontAtlas());
        }
    };

    ObjRenderer::ObjRenderer(VulkanContext& context, const assets::Mesh& mesh, std::filesystem::path shaderDirectory)
        : impl_(std::make_unique<Impl>(context, mesh, std::move(shaderDirectory))) {
    }

    ObjRenderer::~ObjRenderer() = default;

    void ObjRenderer::drawFrame(scene::Camera& camera, RenderSettings& settings) {
        const std::uint32_t width = impl_->context.swapchainWidth();
        const std::uint32_t height = impl_->context.swapchainHeight();
        static_cast<void>(impl_->renderer->drawFrame(impl_->framePacketBuilder.build(
            impl_->level, camera, pipelines::makeDefaultRenderSettingsSnapshot(settings), {},
            core::SurfaceState{.windowExtent = {width, height},
                               .viewportExtent = {width, height},
                               .framebufferResized = false,
                               .minimized = width == 0 || height == 0})));
    }

    void ObjRenderer::waitIdle() const {
        impl_->renderer->waitIdle();
    }

} // namespace lumin::render

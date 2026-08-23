#include "render/presentation/PresentationRenderer.hpp"

#include "render/platform/vulkan/VulkanContext.hpp"

#include <stdexcept>
#include <utility>

namespace lumin::render {

    PresentationRenderer::~PresentationRenderer() {
        shutdown();
    }

    void PresentationRenderer::initialize(VulkanContext& context, ImFontAtlas& fontAtlas, ShaderLibrary& shaders) {
        shutdown();
        renderer_.initialize(UiRendererConfig{
            .device = context.rhiDevice(),
            .colorFormat = context.swapchainRhiFormat(),
            .shaders = &shaders,
            .frameSlotCount = 2,
            .sampleCount = 1,
            .outputIsSrgb = context.swapchainIsSrgb(),
            .fontAtlas = &fontAtlas,
        });

        try {
            framebuffers_.reserve(context.swapchainTextures().size());
            for (const nvrhi::TextureHandle& texture : context.swapchainTextures()) {
                nvrhi::FramebufferHandle framebuffer =
                    context.rhiDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
                if (!framebuffer) {
                    throw std::runtime_error("Failed to create a Presentation swapchain framebuffer.");
                }
                framebuffers_.push_back(std::move(framebuffer));
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    void PresentationRenderer::shutdown() noexcept {
        framebuffers_.clear();
        renderer_.shutdown();
    }

    void PresentationRenderer::setViewportTexture(nvrhi::ITexture* texture) {
        if (texture == nullptr) {
            renderer_.unregisterTexture(viewportTextureId());
        } else {
            renderer_.registerTexture(viewportTextureId(), texture);
        }
    }

    void PresentationRenderer::record(nvrhi::ICommandList& commandList, std::uint32_t imageIndex,
                                      std::uint32_t frameSlot, const ImDrawData& drawData) {
        if (imageIndex >= framebuffers_.size()) {
            throw std::out_of_range("Presentation image index is outside the current swapchain.");
        }
        renderer_.render(commandList, *framebuffers_[imageIndex], frameSlot, drawData);
    }

    nvrhi::ITexture* PresentationRenderer::fontTexture() const noexcept {
        return renderer_.fontTexture();
    }

    nvrhi::ResourceStates PresentationRenderer::fontTextureInitialState() const noexcept {
        return renderer_.fontTextureInitialState();
    }

} // namespace lumin::render

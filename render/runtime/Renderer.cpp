#include "render/Renderer.hpp"

#include "render/platform/vulkan/VulkanContext.hpp"
#include "render/runtime/PresentationFrameRateTracker.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace lumin::render {

    bool RendererViewportStatus::isValid() const noexcept {
        return textureId.isValid() && width != 0 && height != 0;
    }

    bool RendererStatusSnapshot::isReady() const noexcept {
        return state == RendererState::Ready;
    }

    struct Renderer::Impl final {
        Impl(std::unique_ptr<VulkanSurfaceBootstrap> bootstrap, world::RenderWorldSnapshotPtr initialWorld,
             std::filesystem::path shaderDirectory, ImFontAtlas& uiFontAtlas,
             std::unique_ptr<runtime::IRenderPipelineSessionFactory> pipelineFactory) {
            if (bootstrap == nullptr || initialWorld == nullptr || pipelineFactory == nullptr) {
                throw std::invalid_argument(
                    "Renderer requires a Vulkan surface bootstrap, initial world and pipeline factory.");
            }
            context = std::make_unique<VulkanContext>(std::move(*bootstrap));
            session = pipelineFactory->create(runtime::RenderPipelineSessionCreateContext{
                .vulkan = context.get(),
                .initialWorld = std::move(initialWorld),
                .shaderDirectory = std::move(shaderDirectory),
                .uiFontAtlas = &uiFontAtlas,
            });
            if (session == nullptr) {
                throw std::runtime_error("Render pipeline session factory returned null.");
            }
            presentationFrameRate.reset(runtime::PresentationFrameRateTracker::Clock::now());
            publishRuntimeDetails();
        }

        ~Impl() {
            shutdownNoThrow();
        }

        void publishRuntimeDetails() {
            const runtime::RenderPipelineSessionStatus details = session->status();
            status.modelCount = details.modelCount;
            status.mdiDrawCount = details.mdiDrawCount;
            status.globalIlluminationBackend = details.globalIlluminationBackend;
            status.globalIlluminationTemporal = details.globalIlluminationTemporal;
            status.hardwareRayTracing = details.hardwareRayTracing;
            status.viewport = {details.viewportTextureId, details.viewportWidth, details.viewportHeight};
            status.diagnostic = details.diagnostic;
        }

        [[nodiscard]] bool drawFrame(core::RenderFramePacket packet, const ImDrawData& ui) {
            if (status.state != RendererState::Ready || session == nullptr) {
                throw std::logic_error("Renderer is not ready for drawing.");
            }
            const core::ClientFrameId clientFrame = packet.clientFrame;
            const bool minimized = packet.surface.minimized || packet.surface.windowExtent.width == 0 ||
                                   packet.surface.windowExtent.height == 0;
            ++status.attemptedFrameCount;
            try {
                const bool submitted = !minimized && session->drawFrame(std::move(packet), ui);
                const auto completedAt = runtime::PresentationFrameRateTracker::Clock::now();
                const std::optional<float> frameRate = submitted
                                                           ? presentationFrameRate.recordPresentedFrame(completedAt)
                                                           : presentationFrameRate.sample(completedAt);
                publishRuntimeDetails();
                if (submitted) {
                    ++status.presentedFrameCount;
                    status.lastRenderedLogicFrame = clientFrame;
                    status.hasSubmittedFrame = true;
                } else {
                    ++status.skippedFrameCount;
                }
                if (frameRate.has_value()) {
                    status.presentedFramesPerSecond = *frameRate;
                }
                return submitted;
            } catch (const std::exception& exception) {
                status.state = RendererState::Failed;
                status.diagnostic = exception.what();
                throw;
            } catch (...) {
                status.state = RendererState::Failed;
                status.diagnostic = "Renderer failed with a non-standard exception.";
                throw;
            }
        }

        void waitIdle() {
            if (session != nullptr) {
                session->waitIdle();
            }
        }

        void shutdown() {
            if (status.state == RendererState::Stopped) {
                return;
            }
            waitIdle();
            session.reset();
            context.reset();
            status.state = RendererState::Stopped;
        }

        void shutdownNoThrow() noexcept {
            try {
                shutdown();
            } catch (...) {
                session.reset();
                context.reset();
                status.state = RendererState::Stopped;
            }
        }

        std::unique_ptr<VulkanContext> context;
        std::unique_ptr<runtime::IRenderPipelineSession> session;
        runtime::PresentationFrameRateTracker presentationFrameRate;
        RendererStatusSnapshot status;
    };

    Renderer::Renderer(std::unique_ptr<VulkanSurfaceBootstrap> bootstrap, world::RenderWorldSnapshotPtr initialWorld,
                       std::filesystem::path shaderDirectory, ImFontAtlas& uiFontAtlas,
                       std::unique_ptr<runtime::IRenderPipelineSessionFactory> pipelineFactory)
        : impl_(std::make_unique<Impl>(std::move(bootstrap), std::move(initialWorld), std::move(shaderDirectory),
                                       uiFontAtlas, std::move(pipelineFactory))) {
    }

    Renderer::~Renderer() = default;

    bool Renderer::drawFrame(core::RenderFramePacket packet, const ImDrawData& ui) {
        return impl_->drawFrame(std::move(packet), ui);
    }

    RendererStatusSnapshot Renderer::status() const {
        return impl_->status;
    }

    void Renderer::waitIdle() {
        impl_->waitIdle();
    }

    void Renderer::shutdown() {
        impl_->shutdown();
    }

} // namespace lumin::render

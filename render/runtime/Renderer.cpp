#include "render/Renderer.hpp"

#include "render/platform/vulkan/VulkanContext.hpp"
#include "render/runtime/RenderMailbox.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lumin::render {
    namespace {

        [[nodiscard]] std::string exceptionMessage(const std::exception_ptr& failure) {
            if (failure == nullptr) {
                return "Unknown renderer failure.";
            }
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& exception) {
                return exception.what();
            } catch (...) {
                return "Renderer failed with a non-standard exception.";
            }
        }

    } // namespace

    bool RendererViewportStatus::isValid() const noexcept {
        return textureId.isValid() && width != 0 && height != 0;
    }

    bool RendererStatusSnapshot::isReady() const noexcept {
        return state == RendererState::Ready;
    }

    struct Renderer::Impl final {
        Impl(std::unique_ptr<VulkanSurfaceBootstrap> bootstrapValue, world::RenderWorldSnapshotPtr initialWorldValue,
             std::filesystem::path shaderDirectoryValue, core::UiFontAtlas uiFontAtlasValue,
             std::unique_ptr<runtime::IRenderPipelineSessionFactory> pipelineFactoryValue)
            : bootstrap(std::move(bootstrapValue)), initialWorld(std::move(initialWorldValue)),
              shaderDirectory(std::move(shaderDirectoryValue)), uiFontAtlas(std::move(uiFontAtlasValue)),
              pipelineFactory(std::move(pipelineFactoryValue)) {
            if (bootstrap == nullptr || initialWorld == nullptr || pipelineFactory == nullptr) {
                throw std::invalid_argument(
                    "Renderer requires a Vulkan surface bootstrap, initial render-world snapshot and pipeline factory.");
            }
            worker = std::thread([this] {
                run();
            });

            std::unique_lock lock{statusMutex};
            statusChanged.wait(lock, [this] {
                return currentStatus.state != RendererState::Starting;
            });
            if (currentStatus.state == RendererState::Failed) {
                const std::exception_ptr failure = startupFailure;
                lock.unlock();
                worker.join();
                std::rethrow_exception(failure);
            }
        }

        ~Impl() {
            stopNoThrow();
        }

        void publishRuntimeDetails(const runtime::IRenderPipelineSession& session) {
            const runtime::RenderPipelineSessionStatus details = session.status();
            std::lock_guard lock{statusMutex};
            currentStatus.modelCount = details.modelCount;
            currentStatus.mdiDrawCount = details.mdiDrawCount;
            currentStatus.globalIlluminationBackend = details.globalIlluminationBackend;
            currentStatus.globalIlluminationTemporal = details.globalIlluminationTemporal;
            currentStatus.hardwareRayTracing = details.hardwareRayTracing;
            currentStatus.viewport = RendererViewportStatus{details.viewportTextureId, details.viewportWidth,
                                                             details.viewportHeight};
            currentStatus.diagnostic = details.diagnostic;
        }

        void publishState(RendererState state, std::string diagnostic = {}) {
            {
                std::lock_guard lock{statusMutex};
                currentStatus.state = state;
                if (!diagnostic.empty()) {
                    currentStatus.diagnostic = std::move(diagnostic);
                }
            }
            statusChanged.notify_all();
        }

        void run() noexcept {
            std::unique_ptr<runtime::IRenderPipelineSession> session;
            std::optional<runtime::RenderMailboxControl> activeControl;
            try {
                // 主线程只完成 instance/surface bootstrap；设备、NvRHI、交换链及其销毁全部归本线程。
                context = std::make_unique<VulkanContext>(std::move(*bootstrap));
                bootstrap.reset();
                session = pipelineFactory->create(runtime::RenderPipelineSessionCreateContext{
                    .vulkan = context.get(),
                    .initialWorld = std::move(initialWorld),
                    .shaderDirectory = std::move(shaderDirectory),
                    .uiFontAtlas = std::move(uiFontAtlas),
                });
                if (session == nullptr) {
                    throw std::runtime_error("Render pipeline session factory returned null.");
                }
                pipelineFactory.reset();
                publishRuntimeDetails(*session);
                publishState(RendererState::Ready);

                while (std::optional<runtime::RenderMailboxWork> work = mailbox.waitNext()) {
                    if (auto* frame = std::get_if<runtime::RenderMailboxFrame>(&*work)) {
                        const core::ClientFrameId clientFrame = frame->packet.clientFrame;
                        const bool minimized = frame->packet.surface.minimized ||
                                               frame->packet.surface.windowExtent.width == 0 ||
                                               frame->packet.surface.windowExtent.height == 0;
                        const bool submitted = !minimized && session->drawFrame(std::move(frame->packet));
                        publishRuntimeDetails(*session);
                        {
                            std::lock_guard lock{statusMutex};
                            ++currentStatus.completedPacketCount;
                            if (submitted) {
                                currentStatus.lastSubmittedClientFrame = clientFrame;
                                currentStatus.hasSubmittedFrame = true;
                            } else {
                                ++currentStatus.skippedPacketCount;
                            }
                        }
                        // 消费序号在全部 Runtime 状态更新后推进，flush 才能观察到完整结果。
                        mailbox.completeFrame(frame->ordinal);
                        continue;
                    }

                    activeControl = std::get<runtime::RenderMailboxControl>(std::move(*work));
                    if (activeControl->kind == runtime::RenderControlKind::Flush) {
                        session->waitIdle();
                        mailbox.completeControl(*activeControl);
                        activeControl.reset();
                        continue;
                    }

                    publishState(RendererState::Stopping);
                    session->waitIdle();
                    session.reset();
                    context.reset();
                    publishState(RendererState::Stopped);
                    mailbox.completeControl(*activeControl);
                    activeControl.reset();
                    mailbox.close();
                    return;
                }
            } catch (...) {
                const std::exception_ptr failure = std::current_exception();
                {
                    std::lock_guard lock{statusMutex};
                    runtimeFailure = failure;
                    if (currentStatus.state == RendererState::Starting) {
                        startupFailure = failure;
                    }
                }
                // 先发布首个异常，再关闭 mailbox，避免提交线程只观察到二次“mailbox closed”错误。
                publishState(RendererState::Failed, exceptionMessage(failure));
                if (activeControl.has_value() && activeControl->completion != nullptr) {
                    try {
                        activeControl->completion->set_exception(failure);
                    } catch (...) {
                        // 保留触发 Runtime 失败的首个异常。
                    }
                }
                mailbox.fail(failure);
                session.reset();
                context.reset();
            }
        }

        void submit(core::RenderFramePacket packet) {
            const auto rethrowRuntimeFailure = [this] {
                std::lock_guard lock{statusMutex};
                if (currentStatus.state == RendererState::Failed && runtimeFailure != nullptr) {
                    std::rethrow_exception(runtimeFailure);
                }
            };
            rethrowRuntimeFailure();
            runtime::RenderMailboxSubmitResult result;
            try {
                result = mailbox.submit(std::move(packet));
            } catch (const std::logic_error&) {
                // mailbox 关闭与状态发布可并发发生；优先把渲染线程首个异常交给调用方。
                rethrowRuntimeFailure();
                throw;
            }
            std::lock_guard lock{statusMutex};
            ++currentStatus.submittedPacketCount;
            if (result.replacedPendingFrame) {
                ++currentStatus.droppedPacketCount;
            }
        }

        [[nodiscard]] RendererStatusSnapshot status() const {
            std::lock_guard lock{statusMutex};
            return currentStatus;
        }

        void flush() {
            std::lock_guard lifecycleLock{lifecycleMutex};
            {
                std::lock_guard statusLock{statusMutex};
                if (currentStatus.state == RendererState::Failed && runtimeFailure != nullptr) {
                    std::rethrow_exception(runtimeFailure);
                }
            }
            const std::shared_future<void> completed = mailbox.enqueueControl(runtime::RenderControlKind::Flush);
            completed.get();
        }

        void stop() {
            std::lock_guard lifecycleLock{lifecycleMutex};
            if (!worker.joinable()) {
                return;
            }
            const RendererState state = status().state;
            if (state == RendererState::Ready) {
                const std::shared_future<void> completed = mailbox.enqueueControl(runtime::RenderControlKind::Stop);
                try {
                    completed.get();
                } catch (...) {
                    // stop 保证 join；Runtime 错误由 status() 和 flush() 对外传播。
                }
            }
            worker.join();
        }

        void stopNoThrow() noexcept {
            try {
                stop();
            } catch (...) {
                if (worker.joinable()) {
                    mailbox.fail(std::current_exception());
                    worker.join();
                }
            }
        }

        std::unique_ptr<VulkanSurfaceBootstrap> bootstrap;
        std::unique_ptr<VulkanContext> context;
        world::RenderWorldSnapshotPtr initialWorld;
        std::filesystem::path shaderDirectory;
        core::UiFontAtlas uiFontAtlas;
        std::unique_ptr<runtime::IRenderPipelineSessionFactory> pipelineFactory;
        runtime::RenderMailbox mailbox;
        mutable std::mutex statusMutex;
        std::condition_variable statusChanged;
        RendererStatusSnapshot currentStatus;
        std::exception_ptr startupFailure;
        std::exception_ptr runtimeFailure;
        std::mutex lifecycleMutex;
        std::thread worker;
    };

    Renderer::Renderer(std::unique_ptr<VulkanSurfaceBootstrap> bootstrap, world::RenderWorldSnapshotPtr initialWorld,
                       std::filesystem::path shaderDirectory, core::UiFontAtlas uiFontAtlas,
                       std::unique_ptr<runtime::IRenderPipelineSessionFactory> pipelineFactory)
        : impl_(std::make_unique<Impl>(std::move(bootstrap), std::move(initialWorld), std::move(shaderDirectory),
                                       std::move(uiFontAtlas), std::move(pipelineFactory))) {
    }

    Renderer::~Renderer() = default;

    void Renderer::submit(core::RenderFramePacket packet) {
        impl_->submit(std::move(packet));
    }

    RendererStatusSnapshot Renderer::status() const {
        return impl_->status();
    }

    void Renderer::flush() {
        impl_->flush();
    }

    void Renderer::stop() {
        impl_->stop();
    }

} // namespace lumin::render

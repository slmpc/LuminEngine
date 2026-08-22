#include "render/runtime/RenderMailbox.hpp"

#include <stdexcept>
#include <utility>

namespace lumin::render::runtime {

    RenderMailboxSubmitResult RenderMailbox::submit(core::RenderFramePacket packet) {
        std::lock_guard lock{mutex_};
        if (!acceptingFrames_ || closed_ || failure_ != nullptr) {
            throw std::logic_error("Render mailbox is not accepting frame packets.");
        }
        const bool replaced = latestFrame_.has_value();
        const std::uint64_t ordinal = nextFrameOrdinal_++;
        latestFrame_ = RenderMailboxFrame{ordinal, std::move(packet)};
        changed_.notify_one();
        return RenderMailboxSubmitResult{ordinal, replaced};
    }

    std::shared_future<void> RenderMailbox::enqueueControl(RenderControlKind kind) {
        std::lock_guard lock{mutex_};
        if (closed_ || failure_ != nullptr || !acceptingFrames_) {
            throw std::logic_error("Render mailbox cannot enqueue another control command.");
        }

        auto completion = std::make_shared<std::promise<void>>();
        std::shared_future<void> future = completion->get_future().share();
        controls_.push_back(PendingControl{
            .kind = kind,
            .ordinal = nextControlOrdinal_++,
            .requiredFrameOrdinal = nextFrameOrdinal_ - 1,
            .completion = std::move(completion),
        });
        if (kind == RenderControlKind::Stop) {
            // stop 排队后禁止新 frame 越过该退出边界。
            acceptingFrames_ = false;
        }
        changed_.notify_one();
        return future;
    }

    std::optional<RenderMailboxWork> RenderMailbox::waitNext() {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [this] {
            const bool controlReady =
                !controls_.empty() && completedFrameOrdinal_ >= controls_.front().requiredFrameOrdinal;
            return failure_ != nullptr || (closed_ && !latestFrame_.has_value() && controls_.empty()) ||
                   latestFrame_.has_value() || controlReady;
        });
        if (failure_ != nullptr || (closed_ && !latestFrame_.has_value() && controls_.empty())) {
            return std::nullopt;
        }

        if (!controls_.empty() && completedFrameOrdinal_ >= controls_.front().requiredFrameOrdinal) {
            PendingControl control = std::move(controls_.front());
            controls_.pop_front();
            return RenderMailboxControl{
                .kind = control.kind,
                .ordinal = control.ordinal,
                .requiredFrameOrdinal = control.requiredFrameOrdinal,
                .completion = std::move(control.completion),
            };
        }

        RenderMailboxFrame frame = std::move(*latestFrame_);
        latestFrame_.reset();
        return frame;
    }

    void RenderMailbox::completeFrame(std::uint64_t ordinal) noexcept {
        std::lock_guard lock{mutex_};
        if (ordinal > completedFrameOrdinal_) {
            completedFrameOrdinal_ = ordinal;
        }
        changed_.notify_all();
    }

    void RenderMailbox::completeControl(const RenderMailboxControl& control) noexcept {
        if (control.completion == nullptr) {
            return;
        }
        try {
            control.completion->set_value();
        } catch (...) {
            // promise 已满足只表示调用方逻辑错误，不能破坏渲染线程退出。
        }
    }

    void RenderMailbox::fail(std::exception_ptr failure) noexcept {
        std::lock_guard lock{mutex_};
        if (failure_ != nullptr) {
            return;
        }
        if (failure == nullptr) {
            failure = std::make_exception_ptr(std::runtime_error("Renderer runtime failed."));
        }
        failure_ = failure;
        acceptingFrames_ = false;
        closed_ = true;
        latestFrame_.reset();
        for (PendingControl& control : controls_) {
            try {
                control.completion->set_exception(failure_);
            } catch (...) {
                // 保留首个 Runtime 异常，忽略重复满足 promise 的清理错误。
            }
        }
        controls_.clear();
        changed_.notify_all();
    }

    void RenderMailbox::close() noexcept {
        std::lock_guard lock{mutex_};
        acceptingFrames_ = false;
        closed_ = true;
        changed_.notify_all();
    }

} // namespace lumin::render::runtime

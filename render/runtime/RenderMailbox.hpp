#pragma once

#include "render/core/RenderFramePacket.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <variant>

namespace lumin::render::runtime {

    /** 不可丢失控制命令的类型。 */
    enum class RenderControlKind : std::uint8_t {
        /** 等待此前 packet 被消费并等待 GPU idle。 */
        Flush,
        /** 停止接收 packet，排空此前工作并销毁渲染线程资源。 */
        Stop,
    };

    /** 一次 latest-wins frame 提交的结果。 */
    struct RenderMailboxSubmitResult {
        /** mailbox 内部分配的单调提交序号。 */
        std::uint64_t ordinal = 0;
        /** 本次提交是否替换了尚未被渲染线程取走的 packet。 */
        bool replacedPendingFrame = false;
    };

    /** 渲染线程从 mailbox 取出的 frame 工作项。 */
    struct RenderMailboxFrame {
        /** mailbox 提交序号。 */
        std::uint64_t ordinal = 0;
        /** 完全拥有的主线程 packet。 */
        core::RenderFramePacket packet;
    };

    /** 渲染线程从独立 FIFO 控制队列取出的工作项。 */
    struct RenderMailboxControl {
        /** 控制命令类型。 */
        RenderControlKind kind = RenderControlKind::Flush;
        /** 控制队列内部单调序号。 */
        std::uint64_t ordinal = 0;
        /** 命令执行前必须已经消费的 frame 提交序号。 */
        std::uint64_t requiredFrameOrdinal = 0;
        /** 调用线程等待的完成信号；只允许渲染线程设置结果。 */
        std::shared_ptr<std::promise<void>> completion;
    };

    /** mailbox 返回给渲染线程的 frame 或控制工作项。 */
    using RenderMailboxWork = std::variant<RenderMailboxFrame, RenderMailboxControl>;

    /**
     * @brief 单槽 latest-wins frame mailbox 与不可丢失 FIFO 控制队列。
     *
     * 所有方法均可跨线程调用。frame 被替换不会触发历史提交；只有消费者完成工作后调用 `completeFrame()`，
     * 依赖该序号的控制命令才可执行。
     */
    class RenderMailbox final {
    public:
        /** 构造可接收 frame 和控制命令的空 mailbox。 */
        RenderMailbox() = default;

        RenderMailbox(const RenderMailbox&) = delete;
        RenderMailbox& operator=(const RenderMailbox&) = delete;

        /**
         * @brief 提交最新 frame，并替换尚未消费的旧 frame。
         * @throws std::logic_error mailbox 已停止接收 frame 时抛出。
         */
        [[nodiscard]] RenderMailboxSubmitResult submit(core::RenderFramePacket packet);

        /**
         * @brief 将不可丢失控制命令加入 FIFO，并返回可等待完成 future。
         * @throws std::logic_error stop 已排队或 mailbox 已关闭时抛出。
         */
        [[nodiscard]] std::shared_future<void> enqueueControl(RenderControlKind kind);

        /**
         * @brief 阻塞直到 frame 或满足前置 frame 序号的控制命令可执行。
         * @return mailbox 正常关闭且不再有工作时返回空。
         */
        [[nodiscard]] std::optional<RenderMailboxWork> waitNext();

        /** 标记 frame 已被消费；这不表示它一定完成 GPU submit。 */
        void completeFrame(std::uint64_t ordinal) noexcept;

        /** 以成功结果完成一个已取出的控制命令。 */
        void completeControl(const RenderMailboxControl& control) noexcept;

        /**
         * @brief 以同一个异常终止 mailbox，并唤醒所有控制调用方。
         * @param failure 非空异常；为空时会替换为通用 Runtime 失败。
         */
        void fail(std::exception_ptr failure) noexcept;

        /** 正常关闭 mailbox；关闭后 `waitNext()` 在排空工作后返回空。 */
        void close() noexcept;

    private:
        struct PendingControl {
            RenderControlKind kind = RenderControlKind::Flush;
            std::uint64_t ordinal = 0;
            std::uint64_t requiredFrameOrdinal = 0;
            std::shared_ptr<std::promise<void>> completion;
        };

        std::mutex mutex_;
        std::condition_variable changed_;
        std::optional<RenderMailboxFrame> latestFrame_;
        std::deque<PendingControl> controls_;
        std::uint64_t nextFrameOrdinal_ = 1;
        std::uint64_t nextControlOrdinal_ = 1;
        std::uint64_t completedFrameOrdinal_ = 0;
        bool acceptingFrames_ = true;
        bool closed_ = false;
        std::exception_ptr failure_;
    };

} // namespace lumin::render::runtime

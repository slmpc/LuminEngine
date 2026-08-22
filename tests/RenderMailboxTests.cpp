#include "render/runtime/RenderMailbox.hpp"

#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

    using namespace lumin::render;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function> void requireThrows(Function&& function, const char* message) {
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    [[nodiscard]] core::RenderFramePacket packet(std::uint64_t frame) {
        core::RenderFramePacket value;
        value.clientFrame = core::ClientFrameId{frame};
        return value;
    }

    void testLatestFrameReplacesPendingPacket() {
        runtime::RenderMailbox mailbox;
        std::uint64_t replacements = 0;
        std::thread producer([&] {
            for (std::uint64_t frame = 0; frame < 100; ++frame) {
                replacements += mailbox.submit(packet(frame)).replacedPendingFrame ? 1 : 0;
            }
        });
        producer.join();

        std::optional<runtime::RenderMailboxWork> work = mailbox.waitNext();
        auto* frame = std::get_if<runtime::RenderMailboxFrame>(&*work);
        require(frame != nullptr && frame->packet.clientFrame.value == 99,
                "The mailbox must retain only the latest unconsumed packet.");
        require(replacements == 99, "Every overwritten pending packet must be reported exactly once.");
        mailbox.completeFrame(frame->ordinal);
        mailbox.close();
        require(!mailbox.waitNext().has_value(), "A closed and drained mailbox must terminate its consumer.");
    }

    void testControlsWaitForPriorFramesAndRemainOrdered() {
        runtime::RenderMailbox mailbox;
        const runtime::RenderMailboxSubmitResult submitted = mailbox.submit(packet(7));
        const std::shared_future<void> flushed = mailbox.enqueueControl(runtime::RenderControlKind::Flush);
        const std::shared_future<void> stopped = mailbox.enqueueControl(runtime::RenderControlKind::Stop);

        std::optional<runtime::RenderMailboxWork> first = mailbox.waitNext();
        auto* frame = std::get_if<runtime::RenderMailboxFrame>(&*first);
        require(frame != nullptr && frame->ordinal == submitted.ordinal,
                "A control command must not overtake the frame submitted before it.");
        require(flushed.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout,
                "Flush must remain pending until its required frame is completed.");
        mailbox.completeFrame(frame->ordinal);

        std::optional<runtime::RenderMailboxWork> second = mailbox.waitNext();
        auto* flush = std::get_if<runtime::RenderMailboxControl>(&*second);
        require(flush != nullptr && flush->kind == runtime::RenderControlKind::Flush,
                "Flush must be the first control command after the frame boundary.");
        mailbox.completeControl(*flush);
        flushed.get();

        std::optional<runtime::RenderMailboxWork> third = mailbox.waitNext();
        auto* stop = std::get_if<runtime::RenderMailboxControl>(&*third);
        require(stop != nullptr && stop->kind == runtime::RenderControlKind::Stop,
                "Stop must retain FIFO order behind an earlier flush.");
        requireThrows<std::logic_error>(
            [&] {
                static_cast<void>(mailbox.submit(packet(8)));
            },
            "A queued stop must close frame submission immediately.");
        mailbox.completeControl(*stop);
        stopped.get();
        mailbox.close();
    }

    void testFailureWakesControlWaiters() {
        runtime::RenderMailbox mailbox;
        const std::shared_future<void> flushed = mailbox.enqueueControl(runtime::RenderControlKind::Flush);
        mailbox.fail(std::make_exception_ptr(std::runtime_error("render-thread-failure")));
        requireThrows<std::runtime_error>(
            [&] {
                flushed.get();
            },
            "A Runtime failure must propagate to blocked control callers.");
        require(!mailbox.waitNext().has_value(), "A failed mailbox must terminate its consumer.");
    }

} // namespace

int main() {
    try {
        testLatestFrameReplacesPendingPacket();
        testControlsWaitForPriorFramesAndRemainOrdered();
        testFailureWakesControlWaiters();
        std::cout << "RenderMailbox PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RenderMailbox FAIL: " << error.what() << '\n';
        return 1;
    }
}

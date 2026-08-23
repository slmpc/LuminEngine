#include "render/runtime/PresentationFrameRateTracker.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

    using lumin::render::runtime::PresentationFrameRateTracker;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] bool nearlyEqual(float left, float right) noexcept {
        return std::abs(left - right) < 0.001f;
    }

    void testCountsOnlyPresentedFrames() {
        const PresentationFrameRateTracker::TimePoint start{};
        PresentationFrameRateTracker tracker{start};
        std::optional<float> rate;

        for (int frame = 1; frame <= 250; ++frame) {
            rate = tracker.recordPresentedFrame(start + std::chrono::milliseconds{frame * 2});
        }

        require(rate.has_value() && nearlyEqual(*rate, 500.0f),
                "A 500 ms window containing 250 presents must report 500 FPS.");
    }

    void testDoesNotPublishPartialWindow() {
        const PresentationFrameRateTracker::TimePoint start{};
        PresentationFrameRateTracker tracker{start};

        for (int frame = 1; frame <= 249; ++frame) {
            require(!tracker.recordPresentedFrame(start + std::chrono::milliseconds{frame * 2}).has_value(),
                    "The tracker must not publish a partial sampling window.");
        }
    }

    void testPublishesZeroWhenPresentationStops() {
        const PresentationFrameRateTracker::TimePoint start{};
        PresentationFrameRateTracker tracker{start};
        static_cast<void>(tracker.recordPresentedFrame(start + std::chrono::milliseconds{500}));

        const std::optional<float> rate = tracker.sample(start + std::chrono::milliseconds{1000});
        require(rate.has_value() && nearlyEqual(*rate, 0.0f),
                "An elapsed window without presents must report zero FPS.");
    }

} // namespace

int main() {
    try {
        testCountsOnlyPresentedFrames();
        testDoesNotPublishPartialWindow();
        testPublishesZeroWhenPresentationStops();
        std::cout << "PresentationFrameRateTracker PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PresentationFrameRateTracker FAIL: " << error.what() << '\n';
        return 1;
    }
}

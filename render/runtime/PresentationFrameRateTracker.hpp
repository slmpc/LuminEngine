#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace lumin::render::runtime {

    /**
     * @brief 按固定时间窗统计渲染主线程完成的交换链呈现帧率。
     *
     * 统计器不读取主线程或 ImGui 时钟；调用方只应在一次 `present` 流程完成后记录一帧。
     */
    class PresentationFrameRateTracker final {
    public:
        /** 单调时钟类型，允许测试传入确定的时间点。 */
        using Clock = std::chrono::steady_clock;
        /** 统计器使用的单调时间点。 */
        using TimePoint = Clock::time_point;
        /** 发布新采样值前至少覆盖的时间长度。 */
        inline static constexpr std::chrono::milliseconds sampleInterval{500};

        /**
         * @brief 创建空统计窗口。
         * @param windowStart 首个统计窗口的起点。
         */
        explicit PresentationFrameRateTracker(TimePoint windowStart = Clock::now()) noexcept
            : windowStart_(windowStart) {
        }

        /**
         * @brief 清空累计帧并从指定时间重新开始采样。
         * @param windowStart 新统计窗口的起点。
         */
        void reset(TimePoint windowStart) noexcept {
            windowStart_ = windowStart;
            presentedFrameCount_ = 0;
        }

        /**
         * @brief 记录一次已经完成的交换链呈现流程，并按需发布新帧率。
         * @param presentedAt 呈现流程返回时的单调时间点。
         * @return 时间窗达到采样长度时返回该窗口的 FPS，否则返回空值。
         */
        [[nodiscard]] std::optional<float> recordPresentedFrame(TimePoint presentedAt) noexcept {
            ++presentedFrameCount_;
            return sample(presentedAt);
        }

        /**
         * @brief 在没有新呈现帧时推进统计窗口，可用于发布零帧率。
         * @param sampledAt 当前单调时间点。
         * @return 时间窗达到采样长度时返回该窗口的 FPS，否则返回空值。
         */
        [[nodiscard]] std::optional<float> sample(TimePoint sampledAt) noexcept {
            const Clock::duration elapsed = sampledAt - windowStart_;
            if (elapsed < sampleInterval) {
                return std::nullopt;
            }

            const float elapsedSeconds = std::chrono::duration<float>(elapsed).count();
            const float framesPerSecond =
                elapsedSeconds > 0.0f ? static_cast<float>(presentedFrameCount_) / elapsedSeconds : 0.0f;
            windowStart_ = sampledAt;
            presentedFrameCount_ = 0;
            return framesPerSecond;
        }

    private:
        /** 当前采样窗口起点，只在渲染主线程访问。 */
        TimePoint windowStart_;
        /** 当前采样窗口内已完成的呈现帧数，只在渲染主线程访问。 */
        std::uint64_t presentedFrameCount_ = 0;
    };

} // namespace lumin::render::runtime

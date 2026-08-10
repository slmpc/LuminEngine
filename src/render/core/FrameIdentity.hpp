#pragma once

#include <compare>
#include <cstdint>
#include <limits>

namespace lumin::render::core {

    /**
     * @brief 标识一组可被 CPU 更新的帧槽资源。
     *
     * 该类型不会隐式转换为整数，避免把帧槽索引误传为交换链图像索引。
     * 默认构造值无效；只有在对应帧槽 fence 已等待完成后，才能更新该索引关联的资源。
     */
    class FrameSlotIndex final {
    public:
        /// 底层无符号整数类型。
        using ValueType = std::uint32_t;

        /// 表示无效帧槽的保留值。
        static constexpr ValueType invalidValue = std::numeric_limits<ValueType>::max();

        /// 构造无效帧槽索引。
        constexpr FrameSlotIndex() noexcept = default;

        /// 从显式整数构造帧槽索引。
        explicit constexpr FrameSlotIndex(ValueType value) noexcept : value_(value) {
        }

        /// 返回底层整数值；调用方应先检查 `isValid()`。
        [[nodiscard]] constexpr ValueType value() const noexcept {
            return value_;
        }

        /// 返回当前索引是否指向有效帧槽。
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return value_ != invalidValue;
        }

        /// 允许在条件表达式中显式检查有效性。
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return isValid();
        }

        friend constexpr auto operator<=>(const FrameSlotIndex&, const FrameSlotIndex&) noexcept = default;

    private:
        ValueType value_ = invalidValue;
    };

    /**
     * @brief 标识本帧获取到的交换链图像。
     *
     * 该索引只用于 present 图像选择，不表示 CPU 帧槽，也不能与 `FrameSlotIndex` 比较或互换。
     */
    class SwapImageIndex final {
    public:
        /// 底层无符号整数类型。
        using ValueType = std::uint32_t;

        /// 表示无效交换链图像的保留值。
        static constexpr ValueType invalidValue = std::numeric_limits<ValueType>::max();

        /// 构造无效交换链图像索引。
        constexpr SwapImageIndex() noexcept = default;

        /// 从显式整数构造交换链图像索引。
        explicit constexpr SwapImageIndex(ValueType value) noexcept : value_(value) {
        }

        /// 返回底层整数值；调用方应先检查 `isValid()`。
        [[nodiscard]] constexpr ValueType value() const noexcept {
            return value_;
        }

        /// 返回当前索引是否指向有效交换链图像。
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return value_ != invalidValue;
        }

        /// 允许在条件表达式中显式检查有效性。
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return isValid();
        }

        friend constexpr auto operator<=>(const SwapImageIndex&, const SwapImageIndex&) noexcept = default;

    private:
        ValueType value_ = invalidValue;
    };

    /**
     * @brief 标识从引擎启动或渲染序列重启以来的逻辑帧序号。
     *
     * `RenderSequence` 与帧槽数量及交换链图像数量无关，可用于判断历史资源对应的逻辑帧。
     */
    class RenderSequence final {
    public:
        /// 底层无符号整数类型。
        using ValueType = std::uint64_t;

        /// 表示无效渲染序号的保留值。
        static constexpr ValueType invalidValue = std::numeric_limits<ValueType>::max();

        /// 构造无效渲染序号。
        constexpr RenderSequence() noexcept = default;

        /// 从显式整数构造渲染序号。
        explicit constexpr RenderSequence(ValueType value) noexcept : value_(value) {
        }

        /// 返回底层整数值；调用方应先检查 `isValid()`。
        [[nodiscard]] constexpr ValueType value() const noexcept {
            return value_;
        }

        /// 返回当前渲染序号是否有效。
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return value_ != invalidValue;
        }

        /// 允许在条件表达式中显式检查有效性。
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return isValid();
        }

        friend constexpr auto operator<=>(const RenderSequence&, const RenderSequence&) noexcept = default;

    private:
        ValueType value_ = invalidValue;
    };

    /// 描述一次渲染使用的二维像素范围。
    struct RenderExtent {
        /// 水平方向像素数；零表示空范围。
        std::uint32_t width = 0;

        /// 垂直方向像素数；零表示空范围。
        std::uint32_t height = 0;

        /// 返回范围是否为空。
        [[nodiscard]] constexpr bool isEmpty() const noexcept {
            return width == 0 || height == 0;
        }

        /// 以 64 位整数返回像素总数，避免 32 位乘法溢出。
        [[nodiscard]] constexpr std::uint64_t pixelCount() const noexcept {
            return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        }

        friend constexpr bool operator==(const RenderExtent&, const RenderExtent&) noexcept = default;
    };

    /**
     * @brief 汇总一次逻辑渲染帧的稳定身份。
     *
     * 帧槽、交换链图像和逻辑序号具有独立时序，调用方不得从其中任意一个推导另外两个。
     */
    struct RenderFrameIdentity {
        /// 本帧可更新的 CPU/GPU 帧槽。
        FrameSlotIndex frameSlot;

        /// 本帧获取到的交换链图像。
        SwapImageIndex swapImage;

        /// 单调递增的逻辑渲染序号。
        RenderSequence sequence;

        /// 本帧内部渲染范围。
        RenderExtent extent;

        /// 返回所有身份字段是否都已初始化且范围非空。
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return frameSlot.isValid() && swapImage.isValid() && sequence.isValid() && !extent.isEmpty();
        }

        friend constexpr bool operator==(const RenderFrameIdentity&, const RenderFrameIdentity&) noexcept = default;
    };

} // namespace lumin::render::core

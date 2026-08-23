#pragma once

#include <compare>
#include <cstdint>

namespace lumin::render::core {

    /** @brief Dear ImGui 与 Presentation renderer 共享的稳定逻辑纹理 ID。 */
    class UiTextureId final {
    public:
        /** 底层整数类型，可安全装入 `ImTextureID`。 */
        using ValueType = std::uint64_t;
        /** 无效 ID 的保留值。 */
        static constexpr ValueType invalidValue = 0;

        /** 构造无效 ID。 */
        constexpr UiTextureId() noexcept = default;
        /** 从稳定整数构造 ID。 */
        explicit constexpr UiTextureId(ValueType value) noexcept : value_(value) {
        }
        /** 返回底层整数。 */
        [[nodiscard]] constexpr ValueType value() const noexcept {
            return value_;
        }
        /** 返回 ID 是否有效。 */
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return value_ != invalidValue;
        }
        friend constexpr auto operator<=>(const UiTextureId&, const UiTextureId&) noexcept = default;

    private:
        ValueType value_ = invalidValue;
    };

    /** 返回字体图集的保留 ID。 */
    [[nodiscard]] constexpr UiTextureId uiFontTextureId() noexcept {
        return UiTextureId{1};
    }
    /** 返回 Editor Viewport 的保留 ID。 */
    [[nodiscard]] constexpr UiTextureId uiViewportTextureId() noexcept {
        return UiTextureId{2};
    }

} // namespace lumin::render::core

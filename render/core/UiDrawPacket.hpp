#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lumin::render::core {

    /**
     * @brief 跨线程标识 Presentation 纹理的稳定逻辑 ID。
     *
     * ID 不包含 GPU 指针或 binding 地址。Presentation Feature 在渲染线程将其解析为当前物理资源，资源重建不会改变 ID。
     */
    class UiTextureId final {
    public:
        /// 底层整数类型，可安全装入 Dear ImGui 的 `ImTextureID`。
        using ValueType = std::uint64_t;

        /// 表示未指定纹理的保留值；绘制时回退到字体纹理。
        static constexpr ValueType invalidValue = 0;

        /// 构造无效逻辑纹理 ID。
        constexpr UiTextureId() noexcept = default;

        /// 从稳定整数构造逻辑纹理 ID。
        explicit constexpr UiTextureId(ValueType value) noexcept : value_(value) {
        }

        /// 返回底层稳定整数。
        [[nodiscard]] constexpr ValueType value() const noexcept {
            return value_;
        }

        /// 返回 ID 是否引用一个已分配逻辑槽。
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return value_ != invalidValue;
        }

        friend constexpr auto operator<=>(const UiTextureId&, const UiTextureId&) noexcept = default;

    private:
        ValueType value_ = invalidValue;
    };

    /// 返回内置字体图集使用的稳定逻辑纹理 ID。
    [[nodiscard]] constexpr UiTextureId uiFontTextureId() noexcept {
        return UiTextureId{1};
    }

    /// 返回 Editor Viewport 使用的稳定逻辑纹理 ID。
    [[nodiscard]] constexpr UiTextureId uiViewportTextureId() noexcept {
        return UiTextureId{2};
    }

    /// 深拷贝后的单个 Dear ImGui 顶点；布局与 shader 输入 ABI 保持一致。
    struct UiVertex {
        /// framebuffer 逻辑坐标中的 X 位置。
        float positionX = 0.0f;
        /// framebuffer 逻辑坐标中的 Y 位置。
        float positionY = 0.0f;
        /// 纹理 U 坐标。
        float textureU = 0.0f;
        /// 纹理 V 坐标。
        float textureV = 0.0f;
        /// Dear ImGui 打包的 RGBA8 顶点颜色。
        std::uint32_t color = 0;
    };

    /// UI packet 中允许跨线程执行的命令类型。
    enum class UiDrawCommandType : std::uint8_t {
        /// 使用当前状态执行 indexed draw。
        Draw,
        /// 恢复 Presentation renderer 的标准 pipeline、buffer 和 viewport 状态。
        ResetRenderState,
    };

    /// 描述一个不含外部指针的 UI 绘制或状态重置命令。
    struct UiDrawCommand {
        /// 命令类型。
        UiDrawCommandType type = UiDrawCommandType::Draw;
        /// framebuffer 像素坐标中的左裁剪边界。
        std::int32_t scissorLeft = 0;
        /// framebuffer 像素坐标中的上裁剪边界。
        std::int32_t scissorTop = 0;
        /// framebuffer 像素坐标中的右裁剪边界。
        std::int32_t scissorRight = 0;
        /// framebuffer 像素坐标中的下裁剪边界。
        std::int32_t scissorBottom = 0;
        /// 本命令消费的索引数量。
        std::uint32_t elementCount = 0;
        /// packet 全局索引数组中的首个索引。
        std::uint32_t indexOffset = 0;
        /// packet 全局顶点数组中的基准顶点。
        std::uint32_t vertexOffset = 0;
        /// Presentation renderer 需要解析的稳定逻辑纹理 ID。
        UiTextureId texture;
    };

    /**
     * @brief 主线程从 Dear ImGui 深拷贝得到的不可变渲染输入。
     *
     * packet 不保存 ImGui、SDL、Editor、GPU binding 或 user callback 指针。构造完成后可按值移动到渲染线程；
     * 提交后调用方不得继续修改。
     */
    struct UiDrawPacket {
        /// Dear ImGui display 区域左上角 X 坐标。
        float displayPositionX = 0.0f;
        /// Dear ImGui display 区域左上角 Y 坐标。
        float displayPositionY = 0.0f;
        /// Dear ImGui display 区域逻辑宽度。
        float displayWidth = 0.0f;
        /// Dear ImGui display 区域逻辑高度。
        float displayHeight = 0.0f;
        /// 逻辑坐标到 framebuffer 像素的 X 缩放。
        float framebufferScaleX = 1.0f;
        /// 逻辑坐标到 framebuffer 像素的 Y 缩放。
        float framebufferScaleY = 1.0f;
        /// 深拷贝且合并后的顶点数组。
        std::vector<UiVertex> vertices;
        /// 统一转换为 32 位的深拷贝索引数组。
        std::vector<std::uint32_t> indices;
        /// 保持原始顺序的绘制与 reset-state 命令。
        std::vector<UiDrawCommand> commands;

        /// 返回 packet 是否包含可绘制范围和完整几何数据。
        [[nodiscard]] bool isRenderable() const noexcept {
            return displayWidth > 0.0f && displayHeight > 0.0f && framebufferScaleX > 0.0f &&
                   framebufferScaleY > 0.0f && !vertices.empty() && !indices.empty();
        }
    };

    /**
     * @brief 主线程生成并深拷贝的 RGBA8 字体图集。
     *
     * 渲染线程只读取该对象创建字体纹理，不访问 Dear ImGui atlas。像素数量必须等于 `width * height * 4`。
     */
    struct UiFontAtlas {
        /// 图集宽度，单位为像素。
        std::uint32_t width = 0;
        /// 图集高度，单位为像素。
        std::uint32_t height = 0;
        /// 按行紧密排列的 RGBA8 像素。
        std::vector<std::uint8_t> rgba8;

        /// 返回尺寸和像素数量是否构成完整 RGBA8 图集。
        [[nodiscard]] bool isValid() const noexcept {
            if (width == 0 || height == 0) {
                return false;
            }
            constexpr std::uint64_t channels = 4;
            const std::uint64_t required = static_cast<std::uint64_t>(width) * height * channels;
            return required <= std::numeric_limits<std::size_t>::max() && rgba8.size() == required;
        }
    };

} // namespace lumin::render::core

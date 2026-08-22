#pragma once

#include "render/core/FrameDataContracts.hpp"
#include "render/core/RenderSettingsStore.hpp"
#include "render/core/UiDrawPacket.hpp"
#include "render/world/RenderWorld.hpp"

#include <cstdint>

namespace lumin::scene {
    class Camera;
    class Level;
} // namespace lumin::scene

namespace lumin::render::core {

    /** 主线程生成的单调帧标识；它与只在 GPU 提交后推进的 `RenderSequence` 相互独立。 */
    struct ClientFrameId {
        /** 主线程帧序号。 */
        std::uint64_t value = 0;

        friend bool operator==(const ClientFrameId&, const ClientFrameId&) = default;
    };

    /** 主线程观测到的窗口与 Editor Viewport 状态，不保存 SDL 对象或指针。 */
    struct SurfaceState {
        /** SDL 窗口 framebuffer 的物理像素尺寸；零尺寸表示窗口最小化。 */
        RenderExtent windowExtent;
        /** Feature 渲染目标的物理像素尺寸；零尺寸时 Runtime 保持最近有效尺寸。 */
        RenderExtent viewportExtent;
        /** 主线程是否观察到窗口 framebuffer resize 事件。 */
        bool framebufferResized = false;
        /** 单调 surface 修订号；resize packet 被替换后，后续 packet 仍携带尚未应用的代数。 */
        std::uint64_t surfaceRevision = 0;
        /** 窗口是否处于不可渲染的零像素状态。 */
        bool minimized = false;
    };

    /**
     * @brief 主线程提交给渲染 Runtime 的完全拥有、不可变帧消息。
     *
     * Packet 不引用活动 `Level`、`Camera`、Editor、ImGui 或 SDL 状态。提交后调用方必须把它视为不可变值；
     * Runtime 可丢弃尚未消费的旧 packet，但只有成功 GPU 提交才能推进渲染历史。
     */
    struct RenderFramePacket {
        /** 主线程帧身份，仅用于状态关联和丢帧诊断。 */
        ClientFrameId clientFrame;
        /** 当前场景的共享不可变渲染快照。 */
        world::RenderWorldSnapshotPtr world;
        /** 当前相机的值快照；不包含渲染线程决定的 TAA 抖动和上一提交矩阵。 */
        CameraFrameData camera;
        /** 全部 Feature 设置的不可变类型化快照。 */
        RenderSettingsSnapshot settings;
        /** 主线程从 ImGui 深拷贝出的 UI 绘制数据。 */
        UiDrawPacket ui;
        /** 当前窗口和 Viewport 的值状态。 */
        SurfaceState surface;
        /** 相对主线程上一次提取快照的变化提示；Runtime 会相对最近成功提交快照重新计算。 */
        world::SceneChangeMask sceneChangesHint = world::SceneChangeMask::None;

        /** 返回 packet 是否具备可消费的世界、相机尺寸和 Viewport 状态。 */
        [[nodiscard]] bool isValid() const noexcept;
    };

    /**
     * @brief 主线程专用的场景与相机 packet 构建器。
     *
     * Builder 内部缓存最近一次主线程提取结果以复用不可变网格数据；它不提供同步，必须由主线程独占。
     */
    class RenderFramePacketBuilder final {
    public:
        /** 构造从帧 0 开始编号的空 builder。 */
        RenderFramePacketBuilder() = default;

        /**
         * @brief 从活动主线程对象深拷贝一帧消息。
         * @param level 调用期间不得并发修改的活动场景。
         * @param camera 调用期间不得并发修改的活动相机。
         * @param settings 已完成校验的不可变 Feature 设置快照。
         * @param ui 已从 ImGui 深拷贝的绘制 packet；所有权转入结果。
         * @param surface 当前窗口与 Viewport 的值状态。
         * @throws std::invalid_argument Viewport 尺寸无效或设置快照不完整时抛出。
         * @throws std::overflow_error 主线程帧序号耗尽时抛出。
         */
        [[nodiscard]] RenderFramePacket build(const scene::Level& level, const scene::Camera& camera,
                                              RenderSettingsSnapshot settings, UiDrawPacket ui, SurfaceState surface);

        /** 返回最近一次提取的不可变世界快照；首次构建前为空。 */
        [[nodiscard]] world::RenderWorldSnapshotPtr worldSnapshot() const noexcept;

    private:
        world::RenderWorldCache worldCache_;
        std::uint64_t nextClientFrame_ = 0;
    };

} // namespace lumin::render::core

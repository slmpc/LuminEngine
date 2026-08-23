#pragma once

#include "render/core/FrameDataContracts.hpp"
#include "render/core/RenderSettingsStore.hpp"
#include "render/world/RenderWorld.hpp"

#include <cstdint>

namespace lumin::scene {
    class Camera;
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
        /** 单调 surface 修订号；Renderer 用它识别尚未应用的 resize 状态。 */
        std::uint64_t surfaceRevision = 0;
        /** 窗口是否处于不可渲染的零像素状态。 */
        bool minimized = false;
    };

    /**
     * @brief 渲染主线程同步交给 Renderer 的完全拥有帧值。
     *
     * Packet 不引用活动
     * `Level`、`Camera`、Editor、ImGui 或 SDL 状态。`drawFrame()` 按值接收并在返回前消费；
     * 只有成功 GPU
     * 提交才能推进渲染历史。
     */
    struct RenderFramePacket {
        /** 渲染主线程帧身份，仅用于状态关联和跳过帧诊断。 */
        ClientFrameId clientFrame;
        /** 当前场景的共享不可变渲染快照。 */
        world::RenderWorldSnapshotPtr world;
        /** 当前相机的值快照；不包含渲染主线程决定的 TAA 抖动和上一提交矩阵。 */
        CameraFrameData camera;
        /** 全部 Feature 设置的不可变类型化快照。 */
        RenderSettingsSnapshot settings;
        /** 当前窗口和 Viewport 的值状态。 */
        SurfaceState surface;
        /** 相对构建器上一次提取快照的变化提示；session 会相对最近成功提交快照重新计算。 */
        world::SceneChangeMask sceneChangesHint = world::SceneChangeMask::None;

        /** 返回 packet 是否具备可消费的世界、相机尺寸和 Viewport 状态。 */
        [[nodiscard]] bool isValid() const noexcept;
    };

    /**
     * @brief 渲染主线程专用的世界快照与相机 packet 构建器。
     *
     * Builder 只比较逻辑线程已发布的不可变世界快照；它不提供同步，必须由渲染主线程独占。

     */
    class RenderFramePacketBuilder final {
    public:
        /** 构造从帧 0 开始编号的空 builder。 */
        RenderFramePacketBuilder() = default;

        /**
         * @brief 从逻辑世界与渲染线程 Camera 值构建一帧消息。
         * @param world
         * 逻辑线程发布的不可变渲染世界。

         * * @param camera 渲染主线程当前帧使用的 Viewport Camera 值。
         * @param settings
         * 已完成校验的不可变 Feature 设置快照。
         * @param surface 当前窗口与 Viewport 的值状态。
         * @throws std::invalid_argument Viewport 尺寸无效或设置快照不完整时抛出。
         * @throws std::overflow_error 主线程帧序号耗尽时抛出。
         */
        [[nodiscard]] RenderFramePacket build(world::RenderWorldSnapshotPtr world, const scene::Camera& camera,
                                              RenderSettingsSnapshot settings, SurfaceState surface);

        /** 返回最近一次提取的不可变世界快照；首次构建前为空。 */
        [[nodiscard]] world::RenderWorldSnapshotPtr worldSnapshot() const noexcept;

    private:
        world::RenderWorldSnapshotPtr worldSnapshot_;
        std::uint64_t nextClientFrame_ = 0;
    };

} // namespace lumin::render::core

#pragma once

#include "render/core/RenderFramePacket.hpp"
#include "render/core/UiDrawPacket.hpp"
#include "render/gi/GlobalIllumination.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace lumin::render {

    class VulkanContext;

    /** Renderer 异步 Runtime 的生命周期状态。 */
    enum class RendererState : std::uint8_t {
        /** 正在渲染线程执行启动握手。 */
        Starting,
        /** 可接收 frame packet 和控制命令。 */
        Ready,
        /** 已排队 stop，正在排空和销毁 GPU 资源。 */
        Stopping,
        /** 线程和 GPU 资源已经确定性停止。 */
        Stopped,
        /** 启动或逐帧渲染发生不可恢复异常。 */
        Failed,
    };

    /** Editor 主线程可读取的稳定 Viewport 输出状态。 */
    struct RendererViewportStatus {
        /** Presentation Feature 使用的稳定逻辑纹理 ID。 */
        core::UiTextureId textureId;
        /** 当前输出宽度，单位为物理像素。 */
        std::uint32_t width = 0;
        /** 当前输出高度，单位为物理像素。 */
        std::uint32_t height = 0;

        /** 返回逻辑 ID 和尺寸是否均有效。 */
        [[nodiscard]] bool isValid() const noexcept;
    };

    /** 主线程可无锁于 GPU 状态读取的 Renderer 状态值快照。 */
    struct RendererStatusSnapshot {
        /** 当前 Runtime 生命周期状态。 */
        RendererState state = RendererState::Starting;
        /** 已被 `submit()` 接受的 packet 数量。 */
        std::uint64_t submittedPacketCount = 0;
        /** 已由渲染线程消费完成的 packet 数量，包括最小化和 acquire 重试。 */
        std::uint64_t completedPacketCount = 0;
        /** 在被渲染线程取走前由更新 packet 替换的数量。 */
        std::uint64_t droppedPacketCount = 0;
        /** 未推进 GPU 历史的最小化或交换链重试 packet 数量。 */
        std::uint64_t skippedPacketCount = 0;
        /** 最近一次成功 GPU submit 的主线程帧身份。 */
        core::ClientFrameId lastSubmittedClientFrame;
        /** 是否至少存在一次成功 GPU submit。 */
        bool hasSubmittedFrame = false;
        /** 当前 GPU 场景模型数量。 */
        std::uint32_t modelCount = 0;
        /** 当前 multi-draw-indirect draw 数量。 */
        std::uint32_t mdiDrawCount = 0;
        /** 当前实际使用的 GI backend 名称；能力不足时反映 Raster fallback。 */
        std::string globalIlluminationBackend;
        /** 当前 backend 是否维护时序状态。 */
        bool globalIlluminationTemporal = false;
        /** 当前 backend 是否实际使用硬件 Ray Tracing。 */
        bool hardwareRayTracing = false;
        /** Presentation Feature 最近发布的 Viewport 状态。 */
        RendererViewportStatus viewport;
        /** 最近一次不可恢复错误或能力降级诊断；正常状态为空。 */
        std::string diagnostic;

        /** 返回 Runtime 是否仍可接受 frame packet。 */
        [[nodiscard]] bool isReady() const noexcept;
    };

    /**
     * @brief 主线程使用的异步渲染门面。
     *
     * 构造函数完成启动握手后，专用渲染线程独占传入的 `VulkanContext` 及其全部 NvRHI/Vulkan 子资源。
     * `submit()` 使用 latest-wins 语义；`flush()` 与 `stop()` 进入独立 FIFO 控制队列，永不被 frame 替换。
     */
    class Renderer final {
    public:
        /**
         * @brief 创建专用渲染线程并等待 Runtime 初始化完成。
         * @param context 已完成 SDL surface bootstrap 的 Context；所有权立即转入渲染线程。
         * @param initialWorld 完全拥有的初始不可变世界快照。
         * @param shaderDirectory 编译后 shader 目录。
         * @param uiFontAtlas 主线程深拷贝的字体图集。
         * @param globalIllumination 可选 Raster GI backend。
         * @throws std::invalid_argument 必需输入为空时抛出。
         * @throws std::exception 渲染线程启动期间的初始化异常会在构造线程重新抛出。
         */
        Renderer(std::unique_ptr<VulkanContext> context, world::RenderWorldSnapshotPtr initialWorld,
                 std::filesystem::path shaderDirectory, core::UiFontAtlas uiFontAtlas,
                 std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination = {});

        /** 停止渲染线程；析构期间不会传播停止异常。 */
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /**
         * @brief 提交一个完全拥有的最新帧；尚未消费的旧帧会被替换。
         * @throws std::logic_error Runtime 已停止接收帧时抛出。
         */
        void submit(core::RenderFramePacket packet);

        /** 返回当前线程安全状态的按值副本，不访问任何 GPU 对象。 */
        [[nodiscard]] RendererStatusSnapshot status() const;

        /**
         * @brief 等待调用前已提交的 packet 被消费，并在渲染线程等待 GPU idle。
         * @throws std::exception Runtime 失败时重新抛出渲染线程异常。
         */
        void flush();

        /**
         * @brief 禁止后续提交，排空此前 packet，并在渲染线程销毁全部 GPU 资源。
         *
         * 可重复调用；正常返回后状态为 `Stopped`。Runtime 已失败时仍会 join 线程，但错误保留在 `status()`。
         */
        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render

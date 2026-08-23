#pragma once

#include "render/core/RenderFramePacket.hpp"
#include "render/runtime/RenderPipelineSession.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct ImDrawData;
struct ImFontAtlas;

namespace lumin::render {

    class VulkanSurfaceBootstrap;

    /** 同步 Renderer 的生命周期状态。 */
    enum class RendererState : std::uint8_t {
        /** Renderer 已在渲染主线程完成初始化。 */
        Ready,
        /** GPU 资源已经确定性释放。 */
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
        RendererState state = RendererState::Ready;
        /** 已尝试同步绘制的帧数。 */
        std::uint64_t attemptedFrameCount = 0;
        /** 未推进 GPU 历史的最小化或交换链重试帧数。 */
        std::uint64_t skippedFrameCount = 0;
        /** 已完成 GPU submit 和交换链 present 流程的帧数。 */
        std::uint64_t presentedFrameCount = 0;
        /** 渲染主线程按最近呈现帧统计的交换链 FPS。 */
        float presentedFramesPerSecond = 0.0f;
        /** 最近一次成功 GPU submit 的主线程帧身份。 */
        core::ClientFrameId lastRenderedLogicFrame;
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
     * @brief 渲染主线程独占的同步渲染门面。
     *
     * 构造、逐帧绘制和销毁必须发生在同一条拥有 SDL 窗口的线程。
     */
    class Renderer final {
    public:
        /**
         * @brief 在当前渲染主线程创建 Vulkan context 与默认 Pipeline session。
         * @param bootstrap
         * 当前线程完成的 SDL surface bootstrap；所有权转入 Renderer。
         * @param initialWorld
         * 完全拥有的初始不可变世界快照。
         * @param shaderDirectory 编译后 shader 目录。
         * @param uiFontAtlas 当前 ImGui context 的字体图集；引用只在构造期间使用。
         * @param pipelineFactory
         * 显式静态模块组合工厂；所有权转入 Renderer。
         * @throws std::invalid_argument
         * 必需输入为空时抛出。
         * @throws std::exception Vulkan 或 Pipeline session 初始化失败时抛出。
         */
        Renderer(std::unique_ptr<VulkanSurfaceBootstrap> bootstrap, world::RenderWorldSnapshotPtr initialWorld,
                 std::filesystem::path shaderDirectory, ImFontAtlas& uiFontAtlas,
                 std::unique_ptr<runtime::IRenderPipelineSessionFactory> pipelineFactory);

        /** 在当前线程释放 Renderer；析构期间不会传播停止异常。 */
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /**
         * @brief 在当前线程立即记录、提交并呈现一帧。
         * @return 完成 GPU submit 时返回
         * true；最小化或交换链重试时返回 false。
         * @throws std::logic_error Renderer 已停止时抛出。

         */
        [[nodiscard]] bool drawFrame(core::RenderFramePacket packet, const ImDrawData& ui);

        /** 返回当前同步 Renderer 状态的按值副本。 */
        [[nodiscard]] RendererStatusSnapshot status() const;

        /**
         * @brief 在渲染主线程等待当前 GPU 队列 idle。
         */
        void waitIdle();

        /**
         * @brief 在渲染主线程等待 GPU idle 并销毁全部 GPU 资源。
         *
         * 可重复调用；正常返回后状态为
         * `Stopped`。
         */
        void shutdown();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render

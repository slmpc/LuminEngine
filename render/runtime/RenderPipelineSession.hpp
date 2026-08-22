#pragma once

#include "render/core/RenderFramePacket.hpp"
#include "render/core/UiDrawPacket.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace lumin::render {
    class VulkanContext;
}

namespace lumin::render::runtime {

    /** Pipeline session 向异步 Runtime 发布的只读状态。 */
    struct RenderPipelineSessionStatus {
        /** 当前 GPU 场景模型数量。 */
        std::uint32_t modelCount = 0;
        /** 当前 multi-draw-indirect draw 数量。 */
        std::uint32_t mdiDrawCount = 0;
        /** 实际启用的 GI backend 名称。 */
        std::string globalIlluminationBackend;
        /** GI backend 是否拥有时序历史。 */
        bool globalIlluminationTemporal = false;
        /** GI backend 是否实际使用硬件 Ray Tracing。 */
        bool hardwareRayTracing = false;
        /** Presentation Feature 发布的稳定 Viewport 逻辑纹理 ID。 */
        core::UiTextureId viewportTextureId;
        /** Viewport 物理像素宽度。 */
        std::uint32_t viewportWidth = 0;
        /** Viewport 物理像素高度。 */
        std::uint32_t viewportHeight = 0;
        /** 最近一次非致命能力降级或重组诊断。 */
        std::string diagnostic;
    };

    /** 创建一个具体 Pipeline session 所需的完全显式输入。 */
    struct RenderPipelineSessionCreateContext {
        /** 渲染线程独占且由 `Renderer` 拥有的 Vulkan/NvRHI context。 */
        VulkanContext* vulkan = nullptr;
        /** 完全拥有的初始不可变世界快照。 */
        world::RenderWorldSnapshotPtr initialWorld;
        /** 编译后 shader 目录。 */
        std::filesystem::path shaderDirectory;
        /** 主线程深拷贝的 UI 字体图集。 */
        core::UiFontAtlas uiFontAtlas;
    };

    /**
     * @brief 由专用渲染线程独占的一条可执行 Pipeline session。
     *
     * Runtime 只通过该接口提交 immutable packet、等待 GPU 和读取按值状态，不依赖任何具体 Feature 或 recipe。
     */
    class IRenderPipelineSession {
    public:
        IRenderPipelineSession() = default;
        virtual ~IRenderPipelineSession() = default;

        IRenderPipelineSession(const IRenderPipelineSession&) = delete;
        IRenderPipelineSession& operator=(const IRenderPipelineSession&) = delete;

        /**
         * @brief 消费一份完全拥有的帧 packet，并同步完成录制、submit 和 present。
         * @return GPU submit 成功时返回 `true`；最小化或 acquire 重试返回 `false`。
         * @thread_safety 只能由拥有 session 的渲染线程调用。
         */
        [[nodiscard]] virtual bool drawFrame(core::RenderFramePacket packet) = 0;

        /** 在渲染线程等待当前设备队列空闲。 */
        virtual void waitIdle() const = 0;

        /** 返回不引用 GPU 对象的按值状态。 */
        [[nodiscard]] virtual RenderPipelineSessionStatus status() const = 0;
    };

    /** 显式创建具体 Pipeline session 的静态模块组合工厂。 */
    class IRenderPipelineSessionFactory {
    public:
        IRenderPipelineSessionFactory() = default;
        virtual ~IRenderPipelineSessionFactory() = default;

        IRenderPipelineSessionFactory(const IRenderPipelineSessionFactory&) = delete;
        IRenderPipelineSessionFactory& operator=(const IRenderPipelineSessionFactory&) = delete;

        /**
         * @brief 在当前渲染线程创建完整候选 session。
         * @param context 被消费的显式创建输入；工厂不得读取活动 Level、Camera、Editor 或 ImGui 状态。
         * @throws std::exception 任一 Feature 初始化失败时在回滚后继续抛出。
         */
        [[nodiscard]] virtual std::unique_ptr<IRenderPipelineSession>
        create(RenderPipelineSessionCreateContext context) const = 0;
    };

} // namespace lumin::render::runtime

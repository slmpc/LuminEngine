#pragma once

#include "render/core/RenderFramePacket.hpp"
#include "render/editor/ImGuiContent.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>

namespace lumin::render {

    class VulkanContext;

    /**
     * @brief 协调渲染世界快照、延迟渲染 Feature、交换链提交和跨帧状态。
     *
     * 该类型不直接拥有 barrier；所有资源状态转换由 `FrameGraph` 声明。跨帧状态只在
     * `VulkanContext::submitFrameCommands()` 成功返回后统一提交；present 失败不得回滚 GPU 历史。
     */
    class LevelRenderer {
    public:
        /**
         * @brief 使用初始不可变世界快照创建同步 Runtime。
         * @param context 非拥有 Vulkan/NvRHI 上下文；全部调用必须位于其所有线程。
         * @param initialWorld 完全拥有的初始世界快照，用于创建场景相关 GPU 资源。
         * @param shaderDirectory 编译后 shader 目录。
         * @param uiFontAtlas 主线程深拷贝的字体图集。
         * @param globalIllumination 可选 GI backend；为空时创建 Raster fallback。
         */
        LevelRenderer(VulkanContext& context, world::RenderWorldSnapshotPtr initialWorld,
                      std::filesystem::path shaderDirectory, core::UiFontAtlas uiFontAtlas,
                      std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination = {});
        ~LevelRenderer();

        LevelRenderer(const LevelRenderer&) = delete;
        LevelRenderer& operator=(const LevelRenderer&) = delete;

        /**
         * @brief 消费一份完全拥有的帧 packet，并同步录制、提交和 present。
         * @param packet 主线程构建的不可变值；函数返回后不保留对调用方对象的引用。
         */
        void drawFrame(core::RenderFramePacket packet);
        /// 等待当前设备队列空闲。
        void waitIdle() const;

        [[nodiscard]] std::uint32_t modelCount() const noexcept;
        [[nodiscard]] std::uint32_t mdiDrawCount() const noexcept;
        [[nodiscard]] gi::BackendInfo globalIlluminationBackendInfo() const noexcept;
        /** 返回 Presentation Feature 当前发布的稳定 Viewport 逻辑图像。 */
        [[nodiscard]] ImGuiViewportImage viewportImage() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render

#pragma once

#include "render/RenderSettings.hpp"
#include "render/core/UiDrawPacket.hpp"
#include "render/editor/ImGuiContent.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>

namespace lumin::scene {
    class Camera;
    class Level;
} // namespace lumin::scene

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
        /** 创建渲染器并从 `level` 提取首份不可变渲染世界快照。 */
        LevelRenderer(VulkanContext& context, const scene::Level& level, std::filesystem::path shaderDirectory,
                      core::UiFontAtlas uiFontAtlas,
                      std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination = {});
        ~LevelRenderer();

        LevelRenderer(const LevelRenderer&) = delete;
        LevelRenderer& operator=(const LevelRenderer&) = delete;

        /// 提取场景变化并消费主线程深拷贝的 UI packet，随后构建 Feature 管线并提交一帧。
        void drawFrame(scene::Camera& camera, RenderSettings& settings, core::UiDrawPacket uiDrawPacket = {});
        /// 等待当前设备队列空闲。
        void waitIdle() const;

        [[nodiscard]] std::uint32_t modelCount() const noexcept;
        [[nodiscard]] std::uint32_t mdiDrawCount() const noexcept;
        [[nodiscard]] gi::BackendInfo globalIlluminationBackendInfo() const noexcept;
        /** 请求按 Viewport 内容区物理像素重建渲染资源。连续 resize 会等待尺寸稳定后应用。 */
        void requestViewportExtent(std::uint32_t width, std::uint32_t height) noexcept;
        [[nodiscard]] ImGuiViewportImage viewportImage() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render

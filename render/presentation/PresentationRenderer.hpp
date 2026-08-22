#pragma once

#include "render/core/UiDrawPacket.hpp"
#include "render/presentation/UiRenderer.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    class VulkanContext;

    /**
     * @brief 拥有交换链 framebuffer、UI renderer 和逻辑纹理解析的 Presentation 资源。
     *
     * 该类只在渲染线程使用；主线程只持有 `UiTextureId`，不会接触 NvRHI binding。
     */
    class PresentationRenderer final {
    public:
        /// 构造未初始化 Presentation renderer。
        PresentationRenderer() = default;

        /// 释放交换链和 UI GPU 资源。
        ~PresentationRenderer();

        PresentationRenderer(const PresentationRenderer&) = delete;
        PresentationRenderer& operator=(const PresentationRenderer&) = delete;

        /**
         * @brief 为当前交换链和字体图集创建 Presentation 资源。
         * @throws std::invalid_argument 字体图集无效时抛出。
         * @throws std::runtime_error NvRHI 资源创建失败时抛出。
         */
        void initialize(VulkanContext& context, const core::UiFontAtlas& fontAtlas,
                        std::filesystem::path shaderDirectory);

        /// 幂等释放 Presentation 资源。
        void shutdown() noexcept;

        /// 使用稳定 Viewport ID 更新当前物理纹理；空纹理会移除映射。
        void setViewportTexture(nvrhi::ITexture* texture);

        /**
         * @brief 录制 UI packet 到本帧交换链 framebuffer。
         * @throws std::out_of_range `imageIndex` 不属于当前交换链时抛出。
         */
        void record(nvrhi::ICommandList& commandList, std::uint32_t imageIndex, std::uint32_t frameSlot,
                    const core::UiDrawPacket& packet);

        /// 返回字体图集物理纹理。
        [[nodiscard]] nvrhi::ITexture* fontTexture() const noexcept;

        /// 返回字体图集的已上传资源状态。
        [[nodiscard]] nvrhi::ResourceStates fontTextureInitialState() const noexcept;

        /// 返回 Editor Viewport 使用的稳定逻辑纹理 ID。
        [[nodiscard]] static constexpr core::UiTextureId viewportTextureId() noexcept {
            return core::uiViewportTextureId();
        }

    private:
        UiRenderer renderer_;
        std::vector<nvrhi::FramebufferHandle> framebuffers_;
    };

} // namespace lumin::render

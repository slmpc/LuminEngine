#pragma once

#include "render/core/UiTextureId.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

struct ImDrawData;
struct ImFontAtlas;

namespace lumin::render {

    class ShaderLibrary;

    /// 描述 UiRenderer 创建持久 GPU 资源所需的显式输入。
    struct UiRendererConfig {
        /// 渲染主线程独占的 NvRHI 设备。
        nvrhi::IDevice* device = nullptr;
        /// 交换链颜色格式。
        nvrhi::Format colorFormat = nvrhi::Format::UNKNOWN;
        /// Session 级 shader 缓存；生命周期必须覆盖初始化调用。
        ShaderLibrary* shaders = nullptr;
        /// 动态顶点/索引 buffer 的帧槽数量。
        std::uint32_t frameSlotCount = 2;
        /// 交换链采样数。
        std::uint32_t sampleCount = 1;
        /// 目标附件是否执行硬件 sRGB 编码。
        bool outputIsSrgb = false;
        /// 当前渲染主线程拥有的 Dear ImGui 字体图集。
        ImFontAtlas* fontAtlas = nullptr;
    };

    /// UI 顶点位置到 NvRHI 逻辑裁剪空间的线性变换。
    struct UiProjection {
        /// 顶点 X 缩放。
        float scaleX = 0.0f;
        /// 顶点 Y 缩放。
        float scaleY = 0.0f;
        /// 顶点 X 平移。
        float translateX = 0.0f;
        /// 顶点 Y 平移。
        float translateY = 0.0f;
    };

    /**
     * @brief 在渲染主线程把当前 `ImDrawData` 录制为 NvRHI draw 的 Presentation renderer。
     *
     * 该类不保存 ImGui 帧指针；纹理只通过稳定 `UiTextureId` 查找当前 binding。
     */
    class UiRenderer final {
    public:
        /// 构造未初始化 renderer。
        UiRenderer() = default;

        /// 释放全部 GPU 资源。
        ~UiRenderer();

        UiRenderer(const UiRenderer&) = delete;
        UiRenderer& operator=(const UiRenderer&) = delete;

        /**
         * @brief 创建 pipeline、字体纹理和逐帧动态 buffer 槽。
         * @throws std::invalid_argument 配置或字体图集无效时抛出。
         * @throws std::runtime_error NvRHI 资源创建失败时抛出。
         */
        void initialize(const UiRendererConfig& config);

        /// 幂等释放全部 GPU 资源和逻辑纹理映射。
        void shutdown() noexcept;

        /**
         * @brief 将稳定逻辑纹理 ID 映射到当前物理纹理。
         * @throws std::invalid_argument ID 无效、使用字体保留 ID 或纹理为空时抛出。
         * @throws std::runtime_error binding 创建失败时抛出。
         */
        void registerTexture(core::UiTextureId id, nvrhi::ITexture* texture);

        /// 移除逻辑纹理映射；无映射或字体保留 ID 时不执行操作。
        void unregisterTexture(core::UiTextureId id) noexcept;

        /**
         * @brief 使用指定 framebuffer 和帧槽同步录制当前 ImGui draw data。
         * @throws std::logic_error renderer
         * 未初始化或 draw data 引用了未注册纹理时抛出。
         */
        void render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, std::uint32_t frameSlot,
                    const ImDrawData& drawData);

        /// 返回字体图集物理纹理；初始化前为空。
        [[nodiscard]] nvrhi::ITexture* fontTexture() const noexcept;

        /// 返回字体上传完成后的稳定 NvRHI 状态。
        [[nodiscard]] nvrhi::ResourceStates fontTextureInitialState() const noexcept;

        /// 返回 renderer 是否已完成初始化。
        [[nodiscard]] bool initialized() const noexcept;

        /// 按几何增长策略返回能够容纳 `requiredCapacity` 的 buffer 容量。
        [[nodiscard]] static std::size_t growBufferCapacity(std::size_t currentCapacity, std::size_t requiredCapacity,
                                                            std::size_t minimumCapacity) noexcept;

        /// 计算 Dear ImGui display 坐标到 NvRHI 正逻辑 viewport 的投影。
        [[nodiscard]] static UiProjection makeNvrhiProjection(float displayPosX, float displayPosY, float displayWidth,
                                                              float displayHeight) noexcept;

    private:
        struct FrameBuffers {
            nvrhi::BufferHandle vertexBuffer;
            nvrhi::BufferHandle indexBuffer;
            std::size_t vertexCapacity = 0;
            std::size_t indexCapacity = 0;
        };

        void createRendererResources(const UiRendererConfig& config);
        void createFontResources(ImFontAtlas& atlas);
        [[nodiscard]] nvrhi::BindingSetHandle createTextureBinding(nvrhi::ITexture* texture) const;
        [[nodiscard]] nvrhi::IBindingSet& resolveTexture(core::UiTextureId id) const;
        void ensureBuffers(FrameBuffers& buffers, std::size_t vertexCount, std::size_t indexCount);
        void setRenderState(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                            const ImDrawData& drawData, const FrameBuffers& buffers, const nvrhi::Rect& scissor,
                            nvrhi::IBindingSet& textureBinding);

        nvrhi::IDevice* device_ = nullptr;
        ShaderLibrary* shaders_ = nullptr;
        nvrhi::TextureHandle fontTexture_;
        nvrhi::SamplerHandle fontSampler_;
        nvrhi::BindingLayoutHandle bindingLayout_;
        nvrhi::ShaderHandle vertexShader_;
        nvrhi::ShaderHandle fragmentShader_;
        nvrhi::InputLayoutHandle inputLayout_;
        nvrhi::GraphicsPipelineHandle pipeline_;
        std::vector<FrameBuffers> frameBuffers_;
        std::unordered_map<core::UiTextureId::ValueType, nvrhi::BindingSetHandle> textureBindings_;
        bool outputIsSrgb_ = false;
        bool initialized_ = false;
    };

} // namespace lumin::render

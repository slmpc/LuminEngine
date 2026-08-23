#pragma once

#include "render/resources/VulkanResources.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

namespace lumin::render {

    /// Fullscreen set 0 中由上游 Feature 提供的 sampled image 数量。
    inline constexpr std::uint32_t fullscreenSurfaceImageCount = 4;
    /// Fullscreen set 0 中固定的 CSM sampled image 数量。
    inline constexpr std::uint32_t fullscreenShadowImageCount = 4;
    /// Fullscreen set 0 的 sampled image 总数。
    inline constexpr std::uint32_t fullscreenSampledImageCount = 8 + fullscreenShadowImageCount;
    /// Fullscreen set 0 的 sampler binding。
    inline constexpr std::uint32_t fullscreenSamplerBinding = fullscreenSampledImageCount;
    /// Fullscreen set 0 的 post-process constant buffer binding。
    inline constexpr std::uint32_t fullscreenUniformBinding = fullscreenSampledImageCount + 1;

    /** 与内置 Raster/PostFX shader 共享的逐帧常量 ABI。 */
    struct alignas(16) PostProcessUniforms {
        /// 当前非抖动 view-projection 的逆矩阵。
        glm::mat4 inverseViewProjection{1.0f};
        /// 当前带 TAA jitter 的 view-projection。
        glm::mat4 viewProjection{1.0f};
        /// 四个 CSM 级联矩阵。
        std::array<glm::mat4, fullscreenShadowImageCount> cascadeViewProjections{};
        /// 四个 CSM view-space split 距离。
        glm::vec4 cascadeSplits{0.0f};
        /// xyz 为世界空间相机位置。
        glm::vec4 cameraPosition{0.0f};
        /// xyz 为世界空间相机前向。
        glm::vec4 cameraForward{0.0f, 0.0f, -1.0f, 0.0f};
        /// xyz 为世界空间太阳方向，w 表示 direct-lighting Feature 是否启用。
        glm::vec4 lightDirection{-0.45f, -0.8f, -0.35f, 1.0f};
        /// xy 为渲染范围，zw 为其倒数。
        glm::vec4 renderSize{1.0f};
        /// Feature 开关及 TAA 历史有效性，布局由 shader ABI 文档定义。
        glm::vec4 renderOptions{0.0f};
        /// x 为 exposure，y 表示交换链是否为 sRGB，z 为 TAA 后 FSR1 RCAS 锐度。
        glm::vec4 tonemapOptions{1.0f, 0.0f, 0.0f, 0.0f};
        /// x 为 AO mode，y 为世界半径，z 为强度，w 为几何偏置。
        glm::vec4 ambientOcclusionOptions{0.0f, 1.0f, 1.0f, 0.08f};
    };

    static_assert(sizeof(PostProcessUniforms) % 16 == 0);
    static_assert(alignof(PostProcessUniforms) == 16);

    /** Runtime 向 PostFX 显式提供的一个帧槽上游 sampled resources。 */
    struct PostFxBindingInputs {
        /// 顺序固定为 position、normal/roughness、albedo/metallic、motion。
        std::array<nvrhi::TextureHandle, fullscreenSurfaceImageCount> surfaces{};
        /// 四个 CSM sampled depth textures；Hybrid 兼容阶段也必须提供有效占位资源。
        std::array<nvrhi::TextureHandle, fullscreenShadowImageCount> shadows{};
    };

    /** Lighting、Temporal AA 和 Tone Mapping 在一个帧槽中拥有的资源。 */
    struct PostFxFrameResources {
        /// 标准间接光照或 Hybrid direct-radiance scratch。
        GpuTexture globalIllumination;
        /// HDR scene lighting 输出。
        GpuTexture lighting;
        /// Temporal AA resolve 输出。
        GpuTexture taaResolved;
        /// 只在 queue submit 成功后发布的新 TAA history。
        GpuTexture history;
        /// 当前帧槽可写的 shader constant buffer。
        GpuBuffer uniforms;
    };

    /**
     * @brief 独占 Lighting/PostFX 的纹理、TAA 历史和 fullscreen descriptor。
     *
     * 上游表面与阴影资源通过 `PostFxBindingInputs` 注入且不转移所有权。绑定集合强引用这些资源，因此销毁或重建上游
     * Feature 前必须先销毁本对象的资源。更新帧槽常量前调用方必须已等待对应 fence。
     */
    class PostFxResources final {
    public:
        /**
         * @brief 绑定设备并创建空帧槽集合。
         * @param device 生命周期必须覆盖本对象。
         * @param frameSlotCount 并行帧槽数，必须大于零。
         * @throws std::invalid_argument `frameSlotCount` 为零时抛出。
         */
        PostFxResources(nvrhi::IDevice& device, std::uint32_t frameSlotCount);
        /// 释放仍由该 Feature 持有的全部资源。
        ~PostFxResources();

        /// PostFX 资源 owner 不可复制。
        PostFxResources(const PostFxResources&) = delete;
        /// PostFX 资源 owner 不可复制赋值。
        PostFxResources& operator=(const PostFxResources&) = delete;

        /**
         * @brief 事务式创建 PostFX 资源和 descriptor sets。
         * @param width 输出宽度，必须大于零。
         * @param height 输出高度，必须大于零。
         * @param inputs 每个帧槽的上游 sampled handles，数量必须与构造时一致。
         * @throws std::invalid_argument 输入范围或 handle 无效时抛出。
         * @throws std::runtime_error 格式或资源创建失败时抛出。
         */
        void create(std::uint32_t width, std::uint32_t height, std::span<const PostFxBindingInputs> inputs);
        /// 先释放 binding sets，再释放其引用的全部 PostFX 资源；可重复调用。
        void destroy() noexcept;

        /**
         * @brief 将常量写入指定帧槽。
         * @throws std::out_of_range 帧槽索引越界时抛出。
         */
        void updateUniforms(std::uint32_t frameIndex, const PostProcessUniforms& uniforms);
        /// 使全部 TAA 样本无效，但保留历史纹理已经初始化的资源状态。
        void invalidateHistory() noexcept;
        /**
         * @brief 在 queue submit 成功后发布指定帧槽的新历史。
         * @throws std::out_of_range 帧槽索引越界时抛出。
         */
        void markHistoryValid(std::uint32_t frameIndex);

        /**
         * @brief 返回指定帧槽资源。
         * @throws std::out_of_range 帧槽索引越界时抛出。
         */
        [[nodiscard]] const PostFxFrameResources& frame(std::uint32_t frameIndex) const;
        /// 返回帧槽数量。
        [[nodiscard]] std::uint32_t frameSlotCount() const noexcept;
        /** 返回指定帧槽的 TAA 样本是否可参与下一帧混合。 */
        [[nodiscard]] bool historyValid(std::uint32_t frameIndex) const;
        /** 返回指定帧槽的历史纹理是否至少成功写入过一次。 */
        [[nodiscard]] bool historyInitialized(std::uint32_t frameIndex) const;
        /** 返回历史纹理导入 FrameGraph 时必须使用的真实初始状态。 */
        [[nodiscard]] nvrhi::ResourceStates historyInitialState(std::uint32_t frameIndex) const;
        /// 返回 HDR/GI/PostFX 使用的线性高精度格式。
        [[nodiscard]] nvrhi::Format lightingFormat() const noexcept;
        /// 返回标准间接光照输出格式。
        [[nodiscard]] nvrhi::Format globalIlluminationFormat() const noexcept;
        /// 返回 fullscreen sampler；handle 由本对象拥有。
        [[nodiscard]] nvrhi::SamplerHandle sampler() const noexcept;
        /// 返回 fullscreen set 0 layout；handle 由本对象拥有。
        [[nodiscard]] nvrhi::BindingLayoutHandle bindingLayout() const noexcept;
        /**
         * @brief 返回指定帧槽的 fullscreen set 0。
         * @throws std::out_of_range 帧槽索引越界时抛出。
         */
        [[nodiscard]] nvrhi::BindingSetHandle bindingSet(std::uint32_t frameIndex) const;

    private:
        [[nodiscard]] nvrhi::Format chooseFormat(std::span<const nvrhi::Format> candidates,
                                                 nvrhi::FormatSupport required) const;
        [[nodiscard]] GpuTexture createTexture(const nvrhi::TextureDesc& desc) const;
        void createImages(std::uint32_t width, std::uint32_t height);
        void createSamplerAndBindings(std::span<const PostFxBindingInputs> inputs);

        nvrhi::IDevice& device_;
        GpuResourceManager resources_;
        std::vector<PostFxFrameResources> frames_;
        nvrhi::Format globalIlluminationFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format lightingFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::SamplerHandle sampler_;
        nvrhi::BindingLayoutHandle bindingLayout_;
        std::vector<nvrhi::BindingSetHandle> bindingSets_;
        std::vector<bool> historyValid_;
        std::vector<bool> historyInitialized_;
    };

} // namespace lumin::render

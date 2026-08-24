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
    /// Tone Mapping 读取 Bloom HDR 输出的额外 sampled image binding。
    inline constexpr std::uint32_t fullscreenBloomBinding = fullscreenUniformBinding + 1;
    /// Tone Mapping 读取当前自动曝光状态的 sampled image binding。
    inline constexpr std::uint32_t fullscreenAutoExposureBinding = fullscreenBloomBinding + 1;
    /// 固定 Bloom downsample 层级数；每级尺寸向上取整减半。
    inline constexpr std::uint32_t bloomLevelCount = 6;
    /// Bloom upsample 输出数量；最小层直接作为第一次上采样输入。
    inline constexpr std::uint32_t bloomUpsampleLevelCount = bloomLevelCount - 1;

    /** Bloom shader 的 push-constant ABI。 */
    struct alignas(16) BloomPushConstants {
        /// xy 为 source texel size，z 为高光阈值，w 为 soft-knee 比例。
        glm::vec4 filter{0.0f};
        /// x 为合成强度，y 为扩散半径，z 为 pass mode，w 保留。
        glm::vec4 controls{0.0f};
    };

    static_assert(sizeof(BloomPushConstants) == 32);
    static_assert(alignof(BloomPushConstants) == 16);

    /** 自动曝光 shader 的 push-constant ABI。 */
    struct alignas(16) AutoExposurePushConstants {
        /// x/y 为最小/最大 EV，z 为真实帧间隔秒数，w 表示上一成功曝光是否有效。
        glm::vec4 exposureRange{-3.0f, 10.0f, 1.0f / 60.0f, 0.0f};
        /// x/y 为增亮/压暗适应速度，z 表示使用 AgX 测光曲线，w 保留。
        glm::vec4 adaptation{3.0f, 1.0f, 1.0f, 0.0f};
    };

    static_assert(sizeof(AutoExposurePushConstants) == 32);
    static_assert(alignof(AutoExposurePushConstants) == 16);

    /** 与内置 Raster/PostFX shader 共享的逐帧常量 ABI。 */
    struct alignas(16) PostProcessUniforms {
        /// 当前带 TAA jitter 的 view-projection 逆矩阵。
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
        /// x 为 exposure，y 表示交换链是否为 sRGB，z 为 TAA 后 FSR1 RCAS 锐度，w 表示启用 AgX。
        glm::vec4 tonemapOptions{1.0f, 0.0f, 0.0f, 0.0f};
        /// x 为 AO mode，y 为世界半径，z 为强度，w 为几何偏置。
        glm::vec4 ambientOcclusionOptions{0.0f, 1.0f, 1.0f, 0.08f};
        /// xy 为当前帧 screen-UV jitter，zw 为上一成功提交帧的 screen-UV jitter。
        glm::vec4 temporalOptions{0.0f};
        /// x 表示启用自动曝光，y 为曝光补偿 EV，zw 保留。
        glm::vec4 autoExposureOptions{0.0f};
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
        /// Raster GI 或 Hybrid SHARC/NRD composite 的间接光输出。
        GpuTexture globalIllumination;
        /// Hybrid RTDI 独占的直接光 scratch；不得与 `globalIllumination` 或 `lighting` 别名。
        GpuTexture directRadiance;
        /// NRD 恢复材质后的直接光；保留 RTDI 天空 fallback，且不得与其他 lighting UAV 别名。
        GpuTexture denoisedDirectRadiance;
        /// HDR scene lighting 输出。
        GpuTexture lighting;
        /// Temporal AA resolve 输出。
        GpuTexture taaResolved;
        /// 只在 queue submit 成功后发布的新 TAA history。
        GpuTexture history;
        /// 从 TAA HDR 逐级下采样得到的 Bloom 金字塔。
        std::array<GpuTexture, bloomLevelCount> bloomDownsample;
        /// 从最小 Bloom 层逐级累加得到的上采样金字塔。
        std::array<GpuTexture, bloomUpsampleLevelCount> bloomUpsample;
        /// Bloom 合成后的全分辨率 HDR 输出；关闭 Bloom 时保存 TAA 直通副本。
        GpuTexture bloomOutput;
        /// 当前帧槽成功提交后可成为下一帧测光历史的曝光状态；xyz 分别保存平均、保护和最终 EV。
        GpuTexture autoExposure;
        /// Bloom downsample 颜色附件，生命周期与对应纹理一致。
        std::array<nvrhi::FramebufferHandle, bloomLevelCount> bloomDownsampleFramebuffers{};
        /// Bloom upsample 颜色附件，生命周期与对应纹理一致。
        std::array<nvrhi::FramebufferHandle, bloomUpsampleLevelCount> bloomUpsampleFramebuffers{};
        /// 全分辨率 Bloom 合成颜色附件。
        nvrhi::FramebufferHandle bloomOutputFramebuffer;
        /// 每级 downsample 的 source/base/sampler 绑定。
        std::array<nvrhi::BindingSetHandle, bloomLevelCount> bloomDownsampleBindings{};
        /// 每级 upsample 的 lower/base/sampler 绑定。
        std::array<nvrhi::BindingSetHandle, bloomUpsampleLevelCount> bloomUpsampleBindings{};
        /// 最终 TAA 与 Bloom 合成绑定。
        nvrhi::BindingSetHandle bloomCompositeBinding;
        /// 自动曝光 pass 读取当前 Bloom 输出与上一帧槽曝光状态的绑定。
        nvrhi::BindingSetHandle autoExposureBinding;
        /// 自动曝光 1x1 状态输出 framebuffer。
        nvrhi::FramebufferHandle autoExposureFramebuffer;
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
        /// 使自动曝光历史不可参与适应，但保留已初始化纹理的真实资源状态。
        void invalidateAutoExposure() noexcept;
        /**
         * @brief 在 queue submit 成功后发布指定帧槽的新历史。
         * @throws std::out_of_range 帧槽索引越界时抛出。
         */
        void markHistoryValid(std::uint32_t frameIndex);
        /** 在 queue submit 成功后发布指定帧槽的新自动曝光状态。 */
        void markAutoExposureValid(std::uint32_t frameIndex);

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
        /** 返回指定帧槽自动曝光状态是否可作为下一帧历史。 */
        [[nodiscard]] bool autoExposureValid(std::uint32_t frameIndex) const;
        /** 返回指定帧槽自动曝光纹理是否至少成功写入过一次。 */
        [[nodiscard]] bool autoExposureInitialized(std::uint32_t frameIndex) const;
        /** 返回自动曝光纹理导入 FrameGraph 时必须使用的真实初始状态。 */
        [[nodiscard]] nvrhi::ResourceStates autoExposureInitialState(std::uint32_t frameIndex) const;
        /// 返回 HDR/GI/PostFX 使用的线性高精度格式。
        [[nodiscard]] nvrhi::Format lightingFormat() const noexcept;
        /// 返回标准间接光照输出格式。
        [[nodiscard]] nvrhi::Format globalIlluminationFormat() const noexcept;
        /// 返回 fullscreen sampler；handle 由本对象拥有。
        [[nodiscard]] nvrhi::SamplerHandle sampler() const noexcept;
        /// 返回 fullscreen set 0 layout；handle 由本对象拥有。
        [[nodiscard]] nvrhi::BindingLayoutHandle bindingLayout() const noexcept;
        /// 返回 Bloom set 0 layout；handle 由本对象拥有。
        [[nodiscard]] nvrhi::BindingLayoutHandle bloomBindingLayout() const noexcept;
        /// 返回自动曝光 set 0 layout；handle 由本对象拥有。
        [[nodiscard]] nvrhi::BindingLayoutHandle autoExposureBindingLayout() const noexcept;
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
        void createBloomBindings();
        void createAutoExposureBindings();

        nvrhi::IDevice& device_;
        GpuResourceManager resources_;
        std::vector<PostFxFrameResources> frames_;
        nvrhi::Format globalIlluminationFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format lightingFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::SamplerHandle sampler_;
        nvrhi::BindingLayoutHandle bindingLayout_;
        nvrhi::BindingLayoutHandle bloomBindingLayout_;
        nvrhi::BindingLayoutHandle autoExposureBindingLayout_;
        std::vector<nvrhi::BindingSetHandle> bindingSets_;
        std::vector<bool> historyValid_;
        std::vector<bool> historyInitialized_;
        std::vector<bool> autoExposureValid_;
        std::vector<bool> autoExposureInitialized_;
    };

} // namespace lumin::render

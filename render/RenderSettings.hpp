#pragma once

namespace lumin::render {

    /** 直接光照 Feature 的逐帧配置；表面模型由每个 `scene::Material` 独立选择。 */
    struct DirectLightingFeatureSettings {
        /// 为 `false` 时跳过 direct-lighting pass，但保留 GI 与天空结果。
        bool enabled = true;
    };

    /** Raster 阴影 Feature 的级联阴影配置。 */
    struct ShadowSettings {
        /// 为 `false` 时不录制 CSM pass。
        bool enabled = true;
        /// 0 为均匀分割，1 为对数分割。
        float splitLambda = 0.68f;
        /// CSM 覆盖的最大观察空间距离，不得超过相机 far plane。
        float maxDistance = 200.0f;
    };

    /** 选择场景间接光照的主要实现路径。 */
    enum class GlobalIlluminationMode {
        /// 使用 Raster G-buffer 和屏幕空间 AO。
        Legacy,
        /// 使用 GPU Scene、Ray Tracing、SHARC 与可选 NRD。
        RayTracing,
    };

    /** 选择 Legacy 路径的屏幕空间环境光遮蔽算法。 */
    enum class AmbientOcclusionMode {
        /// 基于旋转采样核的 SSAO。
        Ssao,
        /// 基于屏幕空间地平线搜索的 HBAO。
        Hbao,
        /// 基于切片地平线积分的 GTAO。
        Gtao,
    };

    /** Legacy 与 Ray Tracing 两条路径的 GI Feature 开关。 */
    struct GlobalIlluminationSettings {
        /// 请求使用的 GI 路径；能力不足时 Runtime 回退到 `Legacy`。
        GlobalIlluminationMode mode = GlobalIlluminationMode::RayTracing;
        /// Legacy 屏幕空间 AO 总开关；字段名为兼容现有项目格式而保留。
        bool ssaoEnabled = true;
        /// Legacy 路径使用的 AO 算法。
        AmbientOcclusionMode ambientOcclusionMode = AmbientOcclusionMode::Ssao;
        /// AO 世界空间采样半径，必须为有限正数。
        float ambientOcclusionRadius = 1.0f;
        /// AO 强度，必须为有限非负数。
        float ambientOcclusionStrength = 1.0f;
        /// AO 几何偏置，范围为 `[0, 0.5]`。
        float ambientOcclusionBias = 0.08f;
        /// Ray Tracing 路径是否启用 SHARC radiance cache。
        bool sharcEnabled = true;
        /// Ray Tracing 路径是否启用 NRD 去噪。
        bool nrdEnabled = true;
    };

    /** Temporal AA Feature 的逐帧配置。 */
    struct TemporalAaSettings {
        /// 为 `false` 时禁用历史混合，但仍保持统一后处理输出契约。
        bool enabled = true;
    };

    /** Tone Mapping Feature 的逐帧配置。 */
    struct ToneMappingSettings {
        /// 线性曝光倍率，必须为有限非负数。
        float exposure = 1.0f;
    };

    /** 大气 feature 的渲染质量配置；物理参数属于 scene::SceneEnvironment。 */
    struct AtmosphereRenderSettings {
        /// 是否启用物理大气；关闭时使用程序化环境 fallback。
        bool enabled = true;
        /// 是否在天空合成时应用空中透视。
        bool aerialPerspective = true;
    };

    /**
     * 一帧的 feature 配置快照。
     *
     * 相机、太阳光和大气物理参数不是渲染开关，分别由 scene::Camera 与 scene::SceneEnvironment 提供。
     */
    struct RenderSettings {
        /// 直接光照 Feature 设置。
        DirectLightingFeatureSettings directLighting;
        /// Raster 阴影 Feature 设置。
        ShadowSettings shadows;
        /// 全局光照路径、AO、SHARC 与 NRD 设置。
        GlobalIlluminationSettings globalIllumination;
        /// Temporal AA 设置。
        TemporalAaSettings temporalAa;
        /// Tone mapping 设置。
        ToneMappingSettings toneMapping;
        /// 大气渲染设置。
        AtmosphereRenderSettings atmosphere;
    };

} // namespace lumin::render

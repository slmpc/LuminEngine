#pragma once

namespace lumin::render {

    /** 直接光照 Feature 的逐帧配置；表面模型由每个 `scene::Material` 独立选择。 */
    struct DirectLightingFeatureSettings {
        /// 为 `false` 时跳过 direct-lighting pass，但保留 GI 与天空结果。
        bool enabled = true;
    };

    struct ShadowSettings {
        bool enabled = true;
    };

    enum class GlobalIlluminationMode {
        Auto,
        Ssao,
        RayTracedSharcNrd,
    };

    /** GI feature 的选择与启用状态；算法私有调参由对应 feature 自己拥有。 */
    struct GlobalIlluminationSettings {
        bool enabled = true;
        GlobalIlluminationMode mode = GlobalIlluminationMode::Auto;
    };

    struct TemporalAaSettings {
        bool enabled = true;
    };

    struct ToneMappingSettings {
        float exposure = 1.0f;
    };

    /** 大气 feature 的渲染质量配置；物理参数属于 scene::SceneEnvironment。 */
    struct AtmosphereRenderSettings {
        bool enabled = true;
        bool aerialPerspective = true;
    };

    /**
     * 一帧的 feature 配置快照。
     *
     * 相机、太阳光和大气物理参数不是渲染开关，分别由 scene::Camera 与 scene::SceneEnvironment 提供。
     */
    struct RenderSettings {
        DirectLightingFeatureSettings directLighting;
        ShadowSettings shadows;
        GlobalIlluminationSettings globalIllumination;
        TemporalAaSettings temporalAa;
        ToneMappingSettings toneMapping;
        AtmosphereRenderSettings atmosphere;
    };

} // namespace lumin::render

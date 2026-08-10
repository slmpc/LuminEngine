#pragma once

namespace lumin::render {

    /** 直接光照 Feature 的逐帧配置；表面模型由每个 `scene::Material` 独立选择。 */
    struct DirectLightingFeatureSettings {
        /// 为 `false` 时跳过 direct-lighting pass，但保留 GI 与天空结果。
        bool enabled = true;
    };

    struct ShadowSettings {
        bool enabled = true;
        /// 0 为均匀分割，1 为对数分割。
        float splitLambda = 0.68f;
        /// CSM 覆盖的最大观察空间距离，不得超过相机 far plane。
        float maxDistance = 200.0f;
    };

    enum class GlobalIlluminationMode {
        Legacy,
        RayTracing,
    };

    /** Legacy 与 Ray Tracing 两条路径的 GI Feature 开关。 */
    struct GlobalIlluminationSettings {
        GlobalIlluminationMode mode = GlobalIlluminationMode::RayTracing;
        bool ssaoEnabled = true;
        bool sharcEnabled = true;
        bool nrdEnabled = true;
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

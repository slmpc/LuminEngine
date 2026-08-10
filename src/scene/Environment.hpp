#pragma once

#include <glm/vec3.hpp>

namespace lumin::scene {

    /** 场景中作为太阳使用的方向光。方向表示光线传播方向。 */
    struct DirectionalLight {
        glm::vec3 direction{-0.45f, -0.8f, -0.35f};
        glm::vec3 color{1.0f, 0.94f, 0.82f};
        float illuminanceLux = 110000.0f;
        bool castsShadows = true;
    };

    /**
     * 世界空间与大气物理空间之间的显式映射。
     *
     * 大气物理量统一使用千米。`seaLevelWorldY` 定义局部切平面上的海平面高度，
     * `kilometersPerWorldUnit` 禁止隐式假设一世界单位等于一米。
     */
    struct AtmosphereTransform {
        float kilometersPerWorldUnit = 0.001f;
        float seaLevelWorldY = 0.0f;
    };

    /**
     * 行星大气的物理参数。
     *
     * 半径使用千米，散射/吸收系数使用 1/km。参数 revision 与视图 revision 分离，避免相机移动时重建
     * transmittance 和 multi-scattering LUT。
     */
    struct AtmosphereParameters {
        bool enabled = true;
        float bottomRadiusKm = 6360.0f;
        float topRadiusKm = 6460.0f;
        glm::vec3 rayleighScatteringPerKm{0.005802f, 0.013558f, 0.033100f};
        float rayleighDensityScaleKm = 8.0f;
        glm::vec3 mieScatteringPerKm{0.003996f};
        glm::vec3 mieAbsorptionPerKm{0.000444f};
        float mieDensityScaleKm = 1.2f;
        float miePhaseG = 0.8f;
        glm::vec3 ozoneAbsorptionPerKm{0.000650f, 0.001881f, 0.000085f};
        float ozoneLayerCenterKm = 25.0f;
        float ozoneLayerHalfWidthKm = 15.0f;
        glm::vec3 groundAlbedo{0.3f};
    };

    /**
     * 跨 raster、ray tracing 和 GI 共享的场景环境。
     *
     * 该结构只保存场景事实；具体 LUT 尺寸、采样数和质量档位属于 renderer feature 配置。
     */
    struct SceneEnvironment {
        DirectionalLight sun;
        AtmosphereParameters atmosphere;
        AtmosphereTransform atmosphereTransform;
    };

    /** 返回方向光参数是否有限并满足大气照明约束。 */
    [[nodiscard]] bool validateDirectionalLight(const DirectionalLight& light) noexcept;

    /** 返回世界空间到大气物理空间的映射是否有效。 */
    [[nodiscard]] bool validateAtmosphereTransform(const AtmosphereTransform& transform) noexcept;

    /** 返回大气介质、密度分布和行星半径是否满足物理与数值约束。 */
    [[nodiscard]] bool validateAtmosphereParameters(const AtmosphereParameters& parameters) noexcept;

    /** 返回完整场景环境是否可用于大气常量与 LUT 构建。 */
    [[nodiscard]] bool validateSceneEnvironment(const SceneEnvironment& environment) noexcept;

} // namespace lumin::scene

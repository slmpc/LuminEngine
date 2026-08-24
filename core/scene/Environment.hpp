#pragma once

#include <variant>

#include <glm/vec3.hpp>

namespace lumin::scene {

    /** 场景中作为太阳使用的方向光。方向表示光线传播方向。 */
    struct DirectionalLight {
        glm::vec3 direction{-0.45f, -0.8f, -0.35f};
        glm::vec3 color{1.0f, 0.94f, 0.82f};
        float illuminanceLux = 110000.0f;
        bool castsShadows = true;
    };

    /** 可挂载到 Actor 的全向点光源，光强使用坎德拉。 */
    struct PointLight {
        /** 是否参与渲染世界的有效光源表。 */
        bool enabled = true;
        /** 线性 RGB 光色，各分量必须有限且非负。 */
        glm::vec3 color{1.0f};
        /** 发光强度，单位为坎德拉。 */
        float luminousIntensityCandela = 1000.0f;
        /** 平滑衰减截止距离，单位为 world unit。 */
        float range = 10.0f;
        /** 光线追踪直接光是否发射可见性射线。 */
        bool castsShadows = true;

        friend bool operator==(const PointLight&, const PointLight&) = default;
    };

    /** 可挂载到 Actor 的锥形光源，传播方向为 Actor 本地 `-Z`。 */
    struct SpotLight {
        /** 是否参与渲染世界的有效光源表。 */
        bool enabled = true;
        /** 线性 RGB 光色，各分量必须有限且非负。 */
        glm::vec3 color{1.0f};
        /** 发光强度，单位为坎德拉。 */
        float luminousIntensityCandela = 1000.0f;
        /** 平滑衰减截止距离，单位为 world unit。 */
        float range = 10.0f;
        /** 光线追踪直接光是否发射可见性射线。 */
        bool castsShadows = true;
        /** 完全照明区域的锥体半角，单位为度。 */
        float innerConeAngleDegrees = 20.0f;
        /** 光照衰减到零的锥体半角，单位为度。 */
        float outerConeAngleDegrees = 30.0f;

        friend bool operator==(const SpotLight&, const SpotLight&) = default;
    };

    /** Actor 可选局部光源的统一值类型。 */
    using LocalLight = std::variant<PointLight, SpotLight>;

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

    /** 返回点光源参数是否有限并满足局部光约束。 */
    [[nodiscard]] bool validatePointLight(const PointLight& light) noexcept;

    /** 返回锥形光源参数是否有限并满足强度、范围和锥角约束。 */
    [[nodiscard]] bool validateSpotLight(const SpotLight& light) noexcept;

    /** 返回局部光源变体当前保存的参数是否有效。 */
    [[nodiscard]] bool validateLocalLight(const LocalLight& light) noexcept;

    /** 返回世界空间到大气物理空间的映射是否有效。 */
    [[nodiscard]] bool validateAtmosphereTransform(const AtmosphereTransform& transform) noexcept;

    /** 返回大气介质、密度分布和行星半径是否满足物理与数值约束。 */
    [[nodiscard]] bool validateAtmosphereParameters(const AtmosphereParameters& parameters) noexcept;

    /** 返回完整场景环境是否可用于大气常量与 LUT 构建。 */
    [[nodiscard]] bool validateSceneEnvironment(const SceneEnvironment& environment) noexcept;

} // namespace lumin::scene

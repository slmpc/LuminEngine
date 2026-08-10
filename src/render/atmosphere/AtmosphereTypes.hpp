#pragma once

#include <compare>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "render/core/FrameIdentity.hpp"
#include "scene/Camera.hpp"
#include "scene/Environment.hpp"

namespace lumin::render::atmosphere {

    namespace detail {

        /** 大气签名的内部强类型包装；`Category` 阻止不同输入域之间的隐式比较。 */
        template <typename Category> class Signature final {
        public:
            using ValueType = std::uint64_t;

            constexpr Signature() noexcept = default;

            explicit constexpr Signature(ValueType value) noexcept : value_(value) {
            }

            /** 返回签名的确定性整数值。 */
            [[nodiscard]] constexpr ValueType value() const noexcept {
                return value_;
            }

            /** 零值被保留为无效签名。 */
            [[nodiscard]] constexpr bool isValid() const noexcept {
                return value_ != 0;
            }

            friend constexpr auto operator<=>(const Signature&, const Signature&) noexcept = default;

        private:
            ValueType value_ = 0;
        };

        struct OpticalSignatureCategory;
        struct SurfaceSignatureCategory;
        struct LightingSignatureCategory;
        struct ViewSignatureCategory;

    } // namespace detail

    /** 影响介质光学深度的参数签名。 */
    using OpticalSignature = detail::Signature<detail::OpticalSignatureCategory>;

    /** 影响地表反馈与多次散射的参数签名。 */
    using SurfaceSignature = detail::Signature<detail::SurfaceSignatureCategory>;

    /** 影响天空与空中透视入射光的参数签名。 */
    using LightingSignature = detail::Signature<detail::LightingSignatureCategory>;

    /** 影响视点相关 LUT 的参数签名。 */
    using ViewSignature = detail::Signature<detail::ViewSignatureCategory>;

    /**
     * 构建视点相关大气数据所需的后端无关相机快照。
     *
     * 矩阵保持引擎的世界单位；构建 GPU 常量时会把距离字段显式转换为千米。
     */
    struct AtmosphereViewInput {
        glm::vec3 cameraPositionWorld{0.0f};
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        float nearPlaneWorld = 0.0f;
        float farPlaneWorld = 0.0f;
        core::RenderExtent renderExtent;
    };

    /** 四类输入域的签名快照；天空视图只使用高度，空中透视使用完整相机状态。 */
    struct AtmosphereLutSignatures {
        OpticalSignature optical;
        SurfaceSignature surface;
        LightingSignature lighting;
        ViewSignature skyView;
        ViewSignature aerialPerspective;

        /** 返回所有签名是否均已初始化。 */
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return optical.isValid() && surface.isValid() && lighting.isValid() && skyView.isValid() &&
                   aerialPerspective.isValid();
        }

        friend constexpr bool operator==(const AtmosphereLutSignatures&,
                                         const AtmosphereLutSignatures&) noexcept = default;
    };

    /**
     * 从引擎相机生成不可变的大气视图输入。
     *
     * @throws std::invalid_argument `renderExtent` 为空时抛出。
     */
    [[nodiscard]] AtmosphereViewInput makeAtmosphereViewInput(const scene::Camera& camera,
                                                              core::RenderExtent renderExtent);

    /** 返回视图位置、矩阵、裁剪面和渲染范围是否可用于大气计算。 */
    [[nodiscard]] bool validateAtmosphereViewInput(const AtmosphereViewInput& view) noexcept;

    /**
     * 把局部世界位置映射到以千米表示的行星坐标。
     *
     * 世界 Y 轴视为局部径向上方向；海平面以下的位置会钳制到距地表一米，避免射线起点落入地表。
     *
     * @throws std::invalid_argument 场景环境或世界位置无效时抛出。
     */
    [[nodiscard]] glm::vec3 worldPositionToPlanetKm(const glm::vec3& worldPosition,
                                                    const scene::SceneEnvironment& environment);

    /**
     * 从场景与视图生成 LUT 调度签名。
     *
     * `DirectionalLight::direction` 表示光传播方向；照明签名按归一化后的反方向，即指向太阳的方向构建。
     *
     * @throws std::invalid_argument 场景环境或视图输入无效时抛出。
     */
    [[nodiscard]] AtmosphereLutSignatures makeAtmosphereLutSignatures(const scene::SceneEnvironment& environment,
                                                                      const AtmosphereViewInput& view);

} // namespace lumin::render::atmosphere

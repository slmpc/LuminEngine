#pragma once

#include <cstddef>
#include <type_traits>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "render/atmosphere/AtmosphereTypes.hpp"

namespace lumin::render::atmosphere {

    /**
     * raster、ray tracing 与 compute 大气 pass 共享的 GPU 常量布局。
     *
     * 所有介质距离均使用千米，矩阵保持引擎的 Vulkan 投影约定。字段使用 `vec4` 保证常量缓冲区布局稳定。
     */
    struct alignas(16) AtmosphereGpuConstants {
        /// 世界空间到视图空间的矩阵。
        glm::mat4 view{1.0f};
        /// Vulkan 裁剪空间投影矩阵。
        glm::mat4 projection{1.0f};
        /// 从视图空间还原世界空间的矩阵。
        glm::mat4 inverseView{1.0f};
        /// 从裁剪空间还原视图空间的矩阵。
        glm::mat4 inverseProjection{1.0f};
        /// 从裁剪空间直接还原世界空间的矩阵。
        glm::mat4 inverseViewProjection{1.0f};
        /// `xyz` 为世界空间相机位置，`w` 固定为 1。
        glm::vec4 cameraPositionWorld{0.0f};
        /// `xyz` 为行星坐标系中的相机位置（km），`w` 固定为 1。
        glm::vec4 cameraPlanetPositionKm{0.0f};
        /// `xyz` 为归一化朝日方向，`w` 固定为 0。
        glm::vec4 toSunWorld{0.0f};
        /// `rgb` 为线性太阳颜色，`w` 为照度（lux）。
        glm::vec4 sunColorIlluminanceLux{0.0f};
        /// 依次保存 bottom radius、top radius、厚度及厚度倒数（km）。
        glm::vec4 atmosphereRadiiKm{0.0f};
        /// 依次保存 km/world-unit、海平面 world Y、near km 与 far km。
        glm::vec4 worldMappingAndClipKm{0.0f};
        /// 依次保存 width、height 及两者倒数。
        glm::vec4 renderExtent{0.0f};
        /// `rgb` 为 Rayleigh scattering（1/km），`w` 为 scale height 倒数（1/km）。
        glm::vec4 rayleighScatteringAndInvScaleHeight{0.0f};
        /// `rgb` 为 Mie scattering（1/km），`w` 为 scale height 倒数（1/km）。
        glm::vec4 mieScatteringAndInvScaleHeight{0.0f};
        /// `rgb` 为 Mie absorption（1/km），`w` 为 Henyey-Greenstein `g`。
        glm::vec4 mieAbsorptionAndPhaseG{0.0f};
        /// `rgb` 为 ozone absorption（1/km），`w` 保留。
        glm::vec4 ozoneAbsorptionPerKm{0.0f};
        /// 依次保存 ozone 中心高度、半宽、半宽倒数（km）及保留值。
        glm::vec4 ozoneDensityProfileKm{0.0f};
        /// `rgb` 为地表反照率，`w` 表示大气是否启用。
        glm::vec4 groundAlbedoAndEnabled{0.0f};
    };

    static_assert(std::is_standard_layout_v<AtmosphereGpuConstants>);
    static_assert(sizeof(AtmosphereGpuConstants) == 528);
    static_assert(alignof(AtmosphereGpuConstants) == 16);
    static_assert(offsetof(AtmosphereGpuConstants, view) == 0);
    static_assert(offsetof(AtmosphereGpuConstants, projection) == 64);
    static_assert(offsetof(AtmosphereGpuConstants, inverseView) == 128);
    static_assert(offsetof(AtmosphereGpuConstants, inverseProjection) == 192);
    static_assert(offsetof(AtmosphereGpuConstants, inverseViewProjection) == 256);
    static_assert(offsetof(AtmosphereGpuConstants, cameraPositionWorld) == 320);
    static_assert(offsetof(AtmosphereGpuConstants, cameraPlanetPositionKm) == 336);
    static_assert(offsetof(AtmosphereGpuConstants, toSunWorld) == 352);
    static_assert(offsetof(AtmosphereGpuConstants, sunColorIlluminanceLux) == 368);
    static_assert(offsetof(AtmosphereGpuConstants, atmosphereRadiiKm) == 384);
    static_assert(offsetof(AtmosphereGpuConstants, worldMappingAndClipKm) == 400);
    static_assert(offsetof(AtmosphereGpuConstants, renderExtent) == 416);
    static_assert(offsetof(AtmosphereGpuConstants, rayleighScatteringAndInvScaleHeight) == 432);
    static_assert(offsetof(AtmosphereGpuConstants, mieScatteringAndInvScaleHeight) == 448);
    static_assert(offsetof(AtmosphereGpuConstants, mieAbsorptionAndPhaseG) == 464);
    static_assert(offsetof(AtmosphereGpuConstants, ozoneAbsorptionPerKm) == 480);
    static_assert(offsetof(AtmosphereGpuConstants, ozoneDensityProfileKm) == 496);
    static_assert(offsetof(AtmosphereGpuConstants, groundAlbedoAndEnabled) == 512);

    /**
     * 集中构建大气 GPU 常量并完成世界单位到千米、光传播方向到太阳方向的转换。
     *
     * @throws std::invalid_argument 场景环境、视图或矩阵不可逆时抛出。
     */
    [[nodiscard]] AtmosphereGpuConstants buildAtmosphereGpuConstants(const scene::SceneEnvironment& environment,
                                                                     const AtmosphereViewInput& view);

} // namespace lumin::render::atmosphere

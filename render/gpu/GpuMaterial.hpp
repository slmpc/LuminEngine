#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <glm/vec4.hpp>

#include "scene/Material.hpp"

namespace lumin::render::gpu {

    /**
     * raster 与 ray tracing 共用的 GPU 材质记录。
     *
     * 布局固定为四个 16-byte 向量，便于在 Slang 中声明同构结构。字段语义如下：
     *
     * - `baseColorMetallic`: `rgb = base color`，`a = metallic`；Blinn-Phong 的 `a` 固定为 0。
     * - `specularColorShininess`: `rgb = Blinn-Phong specular color`，`a = shininess`。
     * `surfaceParameters.x` 是 GI 使用的等效粗糙度。
     *
     * `surfaceParameters.y` 是 UV scale。
     *
     * `surfaceParameters.z` 是 normal-map Y sign。
     *
     * `surfaceParameters.w` 是 Blinn-Phong 介质由 IOR 推导的法线入射反射率。
     *
     * `metadata.x` 是 SurfaceModel。
     *
     * `metadata.y` 是 texture descriptor index。
     *
     * `metadata.z` 是 has texture set。
     *
     * `metadata.x` 的数值必须与 `scene::SurfaceModel` 保持一致。保留字段必须写零，以便未来追加语义。
     */
    struct alignas(16) GpuMaterialData {
        glm::vec4 baseColorMetallic{1.0f, 1.0f, 1.0f, 0.0f};
        glm::vec4 specularColorShininess{0.04f, 0.04f, 0.04f, 48.0f};
        glm::vec4 surfaceParameters{0.45f, 1.0f, 1.0f, 0.0f};
        glm::uvec4 metadata{0U};
    };

    static_assert(std::is_standard_layout_v<GpuMaterialData>);
    static_assert(sizeof(GpuMaterialData) == 64);
    static_assert(alignof(GpuMaterialData) == 16);
    static_assert(offsetof(GpuMaterialData, baseColorMetallic) == 0);
    static_assert(offsetof(GpuMaterialData, specularColorShininess) == 16);
    static_assert(offsetof(GpuMaterialData, surfaceParameters) == 32);
    static_assert(offsetof(GpuMaterialData, metadata) == 48);

    /**
     * 返回供 NRD、ray-cone LOD 与通用 GI 使用的感知粗糙度。
     *
     * Blinn-Phong 指数使用 `sqrt(2 / (shininess + 2))` 转换；无效输入会回退到稳定默认值。
     */
    [[nodiscard]] float materialDenoisingRoughness(const scene::Material& material) noexcept;

    /**
     * 将场景材质编码为稳定 GPU ABI。
     *
     * 该函数不分配资源；`textureDescriptorIndex` 由调用方的 descriptor table 决定。所有写入值都会被
     * 规范化为有限值，避免无效编辑器输入污染后续 raster、RT 或 NRD dispatch。
     */
    [[nodiscard]] GpuMaterialData packGpuMaterial(const scene::Material& material,
                                                  std::uint32_t textureDescriptorIndex = 0) noexcept;

} // namespace lumin::render::gpu

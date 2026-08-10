#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include <glm/vec3.hpp>

namespace lumin::scene {

    /**
     * 材质使用的表面反射模型。
     *
     * 枚举值会直接写入 GPU material buffer，并由 raster、ray tracing、SHARC 与 NRD adapter 共享。
     * 因此已有值只能追加，不能重新排序或复用。
     */
    enum class SurfaceModel : std::uint32_t {
        /// 金属度-粗糙度工作流，使用 GGX 微表面 BRDF。
        MetallicRoughness = 0,
        /// 传统 Blinn-Phong 漫反射与半程向量高光模型。
        BlinnPhong = 1,
    };

    /** 金属度-粗糙度表面参数。 */
    struct MetallicRoughnessParameters {
        /// 感知粗糙度；上传 GPU 时会限制到有效范围。
        float roughness = 0.45f;
        /// 金属度；上传 GPU 时会限制到 `[0, 1]`。
        float metallic = 0.0f;

        friend bool operator==(const MetallicRoughnessParameters&, const MetallicRoughnessParameters&) = default;
    };

    /** 传统 Blinn-Phong 表面参数。 */
    struct BlinnPhongParameters {
        /// 镜面反射颜色；非金属材质通常使用较暗的中性色。
        glm::vec3 specularColor{0.04f};
        /// 半程向量高光指数；数值越大，高光越集中。
        float shininess = 48.0f;

        friend bool operator==(const BlinnPhongParameters&, const BlinnPhongParameters&) = default;
    };

    /**
     * 当前材质纹理集合。
     *
     * `roughness` 纹理对两种模型都调制逐像素高光宽度：PBR 直接使用感知粗糙度，Blinn-Phong 则从
     * 等效粗糙度反算高光指数。normal 与 base color 同样由两种表面模型共享。
     */
    struct PbrTextureSet {
        std::filesystem::path baseColor;
        std::filesystem::path normal;
        std::filesystem::path roughness;
        bool flipNormalY = true;

        /** 只比较会改变 descriptor image 绑定的路径，不比较采样约定。 */
        [[nodiscard]] bool referencesSameImages(const PbrTextureSet& other) const noexcept {
            return baseColor == other.baseColor && normal == other.normal && roughness == other.roughness;
        }

        friend bool operator==(const PbrTextureSet&, const PbrTextureSet&) = default;
    };

    /**
     * 场景材质的完整、可快照化描述。
     *
     * 两套模型参数会同时保留，因此编辑器切换 `surfaceModel` 时不会丢失另一套调参。渲染阶段只解释当前
     * 模型对应的直接光照参数；GI/NRD 可通过 GPU material contract 读取统一的等效粗糙度。
     */
    struct Material {
        /// 漫反射或基础颜色。
        glm::vec3 albedo{0.82f, 0.68f, 0.48f};
        /// 当前使用的表面模型。
        SurfaceModel surfaceModel = SurfaceModel::MetallicRoughness;
        /// 金属度-粗糙度参数。
        MetallicRoughnessParameters metallicRoughness;
        /// Blinn-Phong 参数。
        BlinnPhongParameters blinnPhong;
        /// 材质 UV 缩放。
        float textureScale = 1.0f;
        /// 可选纹理集合；无纹理时使用 renderer 的零号 fallback descriptor。
        std::optional<PbrTextureSet> textures;

        friend bool operator==(const Material&, const Material&) = default;
    };

    static_assert(static_cast<std::uint32_t>(SurfaceModel::MetallicRoughness) == 0);
    static_assert(static_cast<std::uint32_t>(SurfaceModel::BlinnPhong) == 1);

} // namespace lumin::scene

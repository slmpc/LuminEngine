#pragma once

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include "render/resources/FrameGraph.hpp"

namespace lumin::render::gi {

    /**
     * @brief primary-ray RT 输出的表面信号。
     *
     * 这些纹理由 primary visibility/RTDI pass 写入，不是 raster G-buffer。Hybrid 路径的 SHARC、间接光、
     * NRD、cloud clipping 和 Composite 只能消费这一份契约，避免同一帧出现两套表面重建结果。
     */
    struct RtSurfaceSignalResources {
        /// xyz 为 world position，w 为 camera-to-hit 距离；miss 使用负值。
        nvrhi::TextureHandle worldPositionHitT;
        /// xyz 为 world normal，w 为等效 perceptual roughness。
        nvrhi::TextureHandle normalRoughness;
        /// rgb 为线性 base color，a 为 metallic。
        nvrhi::TextureHandle albedoMetallic;
        /// 稳定 GPU material index；miss 使用 `0xffffffff`。
        nvrhi::TextureHandle materialId;
        /// 沿 camera forward 的正 view-space depth；miss 使用 0。
        nvrhi::TextureHandle viewZ;
        /// screen UV motion，统一为 `previousUv - currentUv`。
        nvrhi::TextureHandle motion;
        /// RT direct lighting；背景像素为 atmosphere miss radiance。
        nvrhi::TextureHandle directRadiance;
        /// rgb 为未解调的直接光漫反射波瓣，a 为本次 shadow ray 的 world-space 命中距离。
        nvrhi::TextureHandle directDiffuseRadianceHitT;
        /// rgb 为未解调的直接光镜面波瓣，a 为本次 shadow ray 的 world-space 命中距离。
        nvrhi::TextureHandle directSpecularRadianceHitT;
        /// 1 表示 primary hit，0 表示 miss。
        nvrhi::TextureHandle visibilityMask;

        [[nodiscard]] bool isValid() const noexcept {
            return worldPositionHitT && normalRoughness && albedoMetallic && materialId && viewZ && motion &&
                   directRadiance && directDiffuseRadianceHitT && directSpecularRadianceHitT && visibilityMask;
        }
    };

    /** `RtSurfaceSignalResources` 在当前 FrameGraph 中的唯一资源身份。 */
    struct RtSurfaceSignalGraphResources {
        FrameGraphResourceHandle worldPositionHitT;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        FrameGraphResourceHandle materialId;
        FrameGraphResourceHandle viewZ;
        FrameGraphResourceHandle motion;
        FrameGraphResourceHandle directRadiance;
        FrameGraphResourceHandle directDiffuseRadianceHitT;
        FrameGraphResourceHandle directSpecularRadianceHitT;
        FrameGraphResourceHandle visibilityMask;

        [[nodiscard]] bool isValid() const noexcept {
            return worldPositionHitT.isValid() && normalRoughness.isValid() && albedoMetallic.isValid() &&
                   materialId.isValid() && viewZ.isValid() && motion.isValid() && directRadiance.isValid() &&
                   directDiffuseRadianceHitT.isValid() && directSpecularRadianceHitT.isValid() &&
                   visibilityMask.isValid();
        }
    };

} // namespace lumin::render::gi

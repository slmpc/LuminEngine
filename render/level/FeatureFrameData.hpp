#pragma once

#include "render/RayTracingBuildConfiguration.hpp"
#include "render/atmosphere/AtmosphereLutGpu.hpp"
#include "render/core/FrameDataContracts.hpp"
#include "render/features/postfx/PostFxResources.hpp"
#include "render/features/raster/RasterFeatureResources.hpp"
#include "render/resources/FrameGraphResourceImporter.hpp"

#include <array>
#include <optional>
#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#if LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE
#define LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI 1
#include "render/gi/raytracing/RayTracedDirectLighting.hpp"
#include "render/gi/raytracing/RtSurfaceSignals.hpp"
#include "render/gpu/GpuScene.hpp"
#else
#define LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI 0
#endif

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI && defined(LUMIN_HAS_NRD) && LUMIN_HAS_NRD && defined(LUMIN_HAS_SHARC) &&       \
    LUMIN_HAS_SHARC
#define LUMIN_LEVEL_RENDERER_HAS_SHARC_INDIRECT 1
#include "render/gi/raytracing/GiComposite.hpp"
#include "render/gi/raytracing/NrdDenoiser.hpp"
#include "render/gi/raytracing/SharcIndirectLighting.hpp"
#include "render/gi/raytracing/SharcRadianceCache.hpp"
#else
#define LUMIN_LEVEL_RENDERER_HAS_SHARC_INDIRECT 0
#endif

namespace lumin::render {

    /** Feature 在当前 `prepareFrame()` 期间共享的物理资源导入服务。 */
    struct FrameImportServices {
        /// 非拥有指针；只在当前 Feature pass 构建阶段有效。
        FrameGraphResourceImporter* importer = nullptr;
    };

    /** Raster Feature 在当前帧槽使用的预创建 framebuffer。 */
    struct RasterPassTargets {
        /// 每个 CSM cascade 对应的纯深度 framebuffer。
        std::array<nvrhi::FramebufferHandle, shadowCascadeCount> shadowFramebuffers{};
        /// 当前帧槽的 MRT G-buffer framebuffer。
        nvrhi::FramebufferHandle surfaceFramebuffer;
    };

    /** Lighting 与 PostFX Feature 在当前帧槽共享的常量和输出 framebuffer。 */
    struct PostProcessPassData {
        /// 本帧写入 frame-slot constant buffer 的完整后处理常量。
        PostProcessUniforms uniforms;
        /// 最近一次成功历史所在的帧槽。
        std::uint32_t historyReadSlot = 0;
        /// HDR lighting 输出 framebuffer。
        nvrhi::FramebufferHandle lightingFramebuffer;
        /// TAA resolve 输出 framebuffer。
        nvrhi::FramebufferHandle temporalFramebuffer;
        /// Tone mapping Viewport 输出 framebuffer。
        nvrhi::FramebufferHandle toneMappingFramebuffer;
    };

    /** Atmosphere Feature 发布给 Raster sky、RT miss 与 SHARC 的图资源记录。 */
    struct AtmospherePassData {
        /// 当前帧 LUT 纹理、常量和 ready pass；Atmosphere Feature 执行前为空。
        std::optional<atmosphere::AtmosphereLutGraphRecord> graphRecord;
    };

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
    /** Hybrid Feature 间传递的 GPU Scene 描述符和候选 pass 状态。 */
    struct HybridPassData {
        /// 当前 recipe 是否为 Hybrid。
        bool active = false;
        /// 本帧是否成功构建了完整 Hybrid GI 链。
        bool globalIlluminationActive = false;
        /// 当前 GPU Scene 的物理 descriptor 集合。
        gpu::GpuSceneDescriptors sceneDescriptors;
        /// 当前 GPU Scene 的几何描述数组。
        std::span<const gpu::GpuGeometryDescriptor> geometry;
        /// TLAS FrameGraph handle。
        FrameGraphResourceHandle tlas;
        /// 实例 buffer FrameGraph handle。
        FrameGraphResourceHandle instances;
        /// 统一光源表 FrameGraph handle。
        FrameGraphResourceHandle lights;
        /// 材质 buffer FrameGraph handle。
        FrameGraphResourceHandle materials;
        /// GPU Scene 完成构建的 pass。
        FrameGraphPassHandle sceneReadyPass;
        /// 几何顶点 buffer handles。
        std::vector<FrameGraphResourceHandle> vertices;
        /// 几何索引 buffer handles。
        std::vector<FrameGraphResourceHandle> indices;
        /// Base-color 纹理 handles。
        std::vector<FrameGraphResourceHandle> baseColorTextures;
        /// Normal/roughness 纹理 handles。
        std::vector<FrameGraphResourceHandle> normalRoughnessTextures;
        /// SHARC 本帧图记录。
#if LUMIN_LEVEL_RENDERER_HAS_SHARC_INDIRECT
        std::optional<gi::SharcGraphRecord> sharcRecord;
        /// SHARC 间接光输出图记录。
        std::optional<gi::SharcIndirectLightingGraphOutput> indirectOutput;
#endif
        /// Primary RT surface graph resources。
        gi::RtSurfaceSignalGraphResources surface;
        /// Primary RT surface ready pass。
        FrameGraphPassHandle surfacePass;
    };
#else
    /** 无 RT 构建中保留的空 Hybrid 状态，使 Raster 源码保持统一。 */
    struct HybridPassData {
        /// 固定为 `false`。
        bool active = false;
        /// 固定为 `false`。
        bool globalIlluminationActive = false;
    };
#endif

} // namespace lumin::render

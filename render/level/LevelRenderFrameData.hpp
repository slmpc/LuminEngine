#pragma once

#include "render/RenderSettings.hpp"
#include "render/atmosphere/AtmosphereLutGpu.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include "render/gpu/GpuScene.hpp"
#include "render/resources/FrameGraph.hpp"
#include "render/resources/TextureManager.hpp"
#include "render/world/RenderWorld.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace lumin::scene {
    class Camera;
}

#if defined(LUMIN_HAS_NRD) && LUMIN_HAS_NRD && defined(LUMIN_HAS_SHARC) && LUMIN_HAS_SHARC
#define LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI 1
#include "render/gi/raytracing/GiComposite.hpp"
#include "render/gi/raytracing/NrdDenoiser.hpp"
#include "render/gi/raytracing/RayTracedDirectLighting.hpp"
#include "render/gi/raytracing/RayTracedGi.hpp"
#include "render/gi/raytracing/RtSurfaceSignals.hpp"
#include "render/gi/raytracing/SharcRadianceCache.hpp"
#else
#define LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI 0
#endif

namespace lumin::render {

    struct LevelCascadeShadowData {
        std::array<glm::mat4, shadowCascadeCount> viewProjections{};
        glm::vec4 splits{0.0f};
    };

    /** 当前 FrameGraph 黑板中共享的逐帧渲染数据。所有引用仅在当前录制/执行调用内有效。 */
    struct LevelRenderFrameData {
        world::RenderWorldSnapshotPtr renderWorldSnapshot;
        const world::RenderWorldSnapshot* renderWorld = nullptr;
        const scene::Camera* camera = nullptr;
        const RenderSettings* settings = nullptr;
        const TextureFrameResources* frame = nullptr;
        std::uint32_t frameIndex = 0;
        std::uint32_t imageIndex = 0;
        std::uint32_t historyReadIndex = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        world::SceneChangeMask sceneChanges = world::SceneChangeMask::None;
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::mat4 viewProjection{1.0f};
        glm::mat4 previousViewProjection{1.0f};
        glm::vec2 jitter{0.0f};
        PostProcessUniforms uniforms;
        LevelCascadeShadowData cascades;
        std::array<FrameGraphResourceHandle, shadowCascadeCount> shadows{};
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normal;
        FrameGraphResourceHandle albedo;
        FrameGraphResourceHandle motion;
        FrameGraphResourceHandle materialId;
        FrameGraphResourceHandle materials;
        FrameGraphResourceHandle depth;
        FrameGraphResourceHandle globalIllumination;
        FrameGraphResourceHandle lighting;
        FrameGraphResourceHandle taaInput;
        FrameGraphResourceHandle taaResolved;
        FrameGraphResourceHandle historyRead;
        FrameGraphResourceHandle historyWrite;
        FrameGraphResourceHandle viewportOutput;
        FrameGraphResourceHandle swap;
        FrameGraphResourceHandle imguiFont;
        std::optional<atmosphere::AtmosphereLutGraphRecord> atmosphereLuts;
        std::array<nvrhi::FramebufferHandle, shadowCascadeCount> shadowFramebuffers{};
        nvrhi::FramebufferHandle gbufferFramebuffer;
        nvrhi::FramebufferHandle lightingFramebuffer;
        nvrhi::FramebufferHandle taaFramebuffer;
        nvrhi::FramebufferHandle tonemapFramebuffer;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        bool hybridGiActive = false;
        gpu::GpuSceneDescriptors hybridSceneDescriptors;
        std::span<const gpu::GpuGeometryDescriptor> hybridGeometry;
        FrameGraphResourceHandle hybridTlas;
        FrameGraphResourceHandle hybridInstances;
        FrameGraphResourceHandle hybridMaterials;
        FrameGraphPassHandle hybridSceneReadyPass;
        std::vector<FrameGraphResourceHandle> hybridVertices;
        std::vector<FrameGraphResourceHandle> hybridIndices;
        std::vector<FrameGraphResourceHandle> hybridBaseColorTextures;
        std::vector<FrameGraphResourceHandle> hybridNormalRoughnessTextures;
        std::optional<gi::SharcGraphRecord> sharcRecord;
        std::optional<gi::RayTracedGiGraphSignals> rayTracedSignals;
        gi::RtSurfaceSignalGraphResources hybridSurface;
        FrameGraphPassHandle hybridSurfacePass;
#endif
        bool hybridPathActive = false;
    };

} // namespace lumin::render

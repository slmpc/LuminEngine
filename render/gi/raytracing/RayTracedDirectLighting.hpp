#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <type_traits>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "render/gi/raytracing/RayTracedGi.hpp"
#include "render/gi/raytracing/RtSurfaceSignals.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#include "render/resources/FrameGraph.hpp"

namespace lumin::render {
    class ShaderLibrary;
}

namespace lumin::render::gi {

    /** 与 `shaders/RtDi.slang` 同构的 primary-ray direct-lighting 常量。 */
    struct alignas(16) RayTracedDiConstants {
        /// 当前帧带 jitter 的 clip-to-world 矩阵。
        glm::mat4 inverseViewProjection{1.0F};
        /// 用于从 RT 命中位置生成 TAA current-previous screen motion。
        glm::mat4 previousViewProjection{1.0F};
        /// xyz 为 world-space 相机位置。
        glm::vec4 cameraPosition{0.0F};
        /// xyz 为 world-space camera forward，用于 viewZ 和 cloud clipping。
        glm::vec4 cameraForward{0.0F, 0.0F, -1.0F, 0.0F};
        /// xyz 为从表面指向太阳的 world-space 单位向量。
        glm::vec4 toSunWorld{0.0F, 1.0F, 0.0F, 0.0F};
        /// rgb 为太阳辐亮度，w 为天空可见性开关。
        glm::vec4 sunRadiance{1.0F};
        /// xy 为 dispatch 分辨率，zw 为当前 jitter 对 screen UV 的偏移；用于与 raster motion 保持一致。
        glm::vec4 renderSize{1.0F};
        /// x=minT，y=maxT，z=直接光照开关，w=逻辑帧序号。
        glm::vec4 traceParameters{0.001F, 10000.0F, 1.0F, 0.0F};
    };

    static_assert(std::is_standard_layout_v<RayTracedDiConstants>);
    static_assert(sizeof(RayTracedDiConstants) == 224);
    static_assert(alignof(RayTracedDiConstants) == 16);
    static_assert(offsetof(RayTracedDiConstants, inverseViewProjection) == 0);
    static_assert(offsetof(RayTracedDiConstants, previousViewProjection) == 64);
    static_assert(offsetof(RayTracedDiConstants, cameraPosition) == 128);
    static_assert(offsetof(RayTracedDiConstants, cameraForward) == 144);
    static_assert(offsetof(RayTracedDiConstants, toSunWorld) == 160);
    static_assert(offsetof(RayTracedDiConstants, sunRadiance) == 176);
    static_assert(offsetof(RayTracedDiConstants, renderSize) == 192);
    static_assert(offsetof(RayTracedDiConstants, traceParameters) == 208);

    /// RTDI 输出的物理表面信号；`viewZ`/`visibilityMask` 可由 pass 在创建时补齐。
    using RayTracedDiFrameResources = RtSurfaceSignalResources;

    /// RTDI 输出的 FrameGraph 身份。
    using RayTracedDiGraphResources = RtSurfaceSignalGraphResources;

    namespace detail {

        /// 构造 RTDI set 0 的 descriptor layout；不包含任何 G-buffer 或 CSM 输入。
        [[nodiscard]] nvrhi::BindingLayoutDesc
        makeRayTracedDiBindingLayoutDesc(std::uint32_t maxGeometryDescriptors,
                                         std::uint32_t maxMaterialTextureDescriptors = 1);

        /// 构造当前 GPU Scene 版本的 RTDI binding set；只包含 TLAS/scene、surface UAV 和 constants。
        [[nodiscard]] nvrhi::BindingSetDesc makeRayTracedDiBindingSetDesc(const RayTracedDiFrameResources& outputs,
                                                                          const RayTracedGiSceneBindings& scene,
                                                                          std::uint32_t maxGeometryDescriptors,
                                                                          std::uint32_t maxMaterialTextureDescriptors,
                                                                          nvrhi::BufferHandle constants);

        template <typename CommandList>
        void recordRayTracedDiDispatch(CommandList& commandList, const nvrhi::rt::State& state, std::uint32_t width,
                                       std::uint32_t height) {
            commandList.setRayTracingState(state);
            commandList.dispatchRays(nvrhi::rt::DispatchRaysArguments().setDimensions(width, height));
        }

    } // namespace detail

    /**
     * 从相机 primary ray 开始的 RT direct-lighting pass。
     *
     * 该 pass 不读取 G-buffer、不读取 CSM，也不拥有 SHARC/NRD 历史。每个 primary 命中点重建材质、写入
     * `RtSurfaceSignals`，并直接执行太阳 RT shadow ray；miss 直接写入 atmosphere environment radiance。
     * RT 不可用时由上层选择原 deferred fallback。
     */
    class RayTracedDirectLightingPass final {
    public:
        struct CreateInfo {
            /** NvRHI 设备；生命周期必须覆盖 pass。 */
            nvrhi::IDevice* device = nullptr;
            /** Session 级 shader 缓存；生命周期必须覆盖创建过程。 */
            ShaderLibrary* shaders = nullptr;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t maxGeometryDescriptors = 0;
            std::uint32_t maxMaterialTextureDescriptors = 0;
            nvrhi::BindingLayoutHandle atmosphereBindingLayout;
            std::span<const RayTracedDiFrameResources> frames;
        };

        explicit RayTracedDirectLightingPass(const CreateInfo& createInfo);
        ~RayTracedDirectLightingPass();

        RayTracedDirectLightingPass(const RayTracedDirectLightingPass&) = delete;
        RayTracedDirectLightingPass& operator=(const RayTracedDirectLightingPass&) = delete;

        /// 在 frame slot fence 完成后记录一帧 primary-ray RTDI。
        [[nodiscard]] FrameGraphPassHandle record(FrameGraph& frameGraph, std::uint32_t frameIndex,
                                                  bool frameSlotFenceWaited, const RayTracedDiConstants& constants,
                                                  const RayTracedDiGraphResources& outputs,
                                                  const RayTracedGiSceneBindings& scene,
                                                  const RayTracedGiSceneGraphResources& sceneResources,
                                                  const RayTracingEnvironmentBindings& environment,
                                                  const RayTracingEnvironmentGraphResources& environmentResources);

        /// queue submit 成功后发布帧槽输出的真实资源状态。
        void commitSubmittedFrame();
        /// 录制、执行或提交失败时放弃当前候选状态。
        void discardPendingFrame() noexcept;
        [[nodiscard]] bool hasPendingFrame() const noexcept;

        /// 返回指定帧槽的 primary RT surface 物理资源。
        [[nodiscard]] const RayTracedDiFrameResources& signals(std::uint32_t frameIndex) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

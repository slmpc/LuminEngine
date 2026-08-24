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

#include "render/gi/raytracing/RtSurfaceSignals.hpp"
#include "render/gi/raytracing/SharcRadianceCache.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#include "render/resources/FrameGraph.hpp"

namespace lumin::render {
    class ShaderLibrary;
}

namespace lumin::scene {
    struct DirectionalLight;
}

namespace lumin::render::gi {

    /** RTDI 使用的当前 GPU Scene 物理 descriptor。 */
    struct RayTracingSceneBindings {
        gpu::GpuSceneDescriptors descriptors;
        std::span<const gpu::GpuGeometryDescriptor> geometry;
        std::span<const nvrhi::TextureHandle> baseColorTextures;
        std::span<const nvrhi::TextureHandle> normalRoughnessTextures;
        nvrhi::SamplerHandle materialSampler;
    };

    /** 必须复用 GPU Scene upload/build 阶段导入的 FrameGraph 资源身份。 */
    struct RayTracingSceneGraphResources {
        FrameGraphResourceHandle tlas;
        FrameGraphResourceHandle instances;
        FrameGraphResourceHandle materials;
        FrameGraphResourceHandle lights;
        std::span<const FrameGraphResourceHandle> vertices;
        std::span<const FrameGraphResourceHandle> indices;
        std::span<const FrameGraphResourceHandle> baseColorTextures;
        std::span<const FrameGraphResourceHandle> normalRoughnessTextures;
        FrameGraphPassHandle readyPass;
    };

    /** 物理光照缓冲采用的固定相机基准曝光值，等价于晴天室外的 EV100 15。 */
    inline constexpr float physicalLightingEv100 = 15.0F;
    /** ISO 100 曝光的饱和归一化系数；预曝光为 `1 / (q * 2^EV100)`。 */
    inline constexpr float physicalExposureSaturationScale = 1.2F;
    /** 将物理照度/辐亮度预曝光到 FP16 scene-linear 缓冲的固定倍率。 */
    inline constexpr float physicalLightingPreExposure = 1.0F / (physicalExposureSaturationScale * 32768.0F);

    /**
     * 将场景太阳照度转换为 Ray Tracing 使用的预曝光 scene-linear 入射照度。
     *
     * RGB 保持 lux 的线性比例并应用 EV100 15 物理预曝光。
     * 该转换不匹配任意 Raster 常量；`w` 始终为 1。
     * 关闭直射光时，atmosphere miss 与间接光仍可读取天空。
     *
     * @param sun 场景拥有的方向光快照，调用期间必须有效。
     * @param directLightingEnabled 是否输出太阳直射光。
     * @return `rgb` 为 RTDI、SHARC update 与间接光 fallback 共用的太阳入射照度。
     */
    [[nodiscard]] glm::vec4 makeRayTracingSunIrradiance(const scene::DirectionalLight& sun,
                                                        bool directLightingEnabled) noexcept;

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
        /// rgb 为预曝光太阳入射照度，w 为天空可见性开关。
        glm::vec4 sunIrradiance{1.0F};
        /// xy 为 dispatch 分辨率，zw 为当前 jitter 对 screen UV 的偏移；用于与 raster motion 保持一致。
        glm::vec4 renderSize{1.0F};
        /// x=minT，y=maxT，z=直接光照开关，w=逻辑帧序号。
        glm::vec4 traceParameters{0.001F, 10000.0F, 1.0F, 0.0F};
        /// x=有效 lightCount，y=成功帧序号，zw 保留。
        glm::uvec4 samplingParameters{1U, 0U, 0U, 0U};
    };

    static_assert(std::is_standard_layout_v<RayTracedDiConstants>);
    static_assert(sizeof(RayTracedDiConstants) == 240);
    static_assert(alignof(RayTracedDiConstants) == 16);
    static_assert(offsetof(RayTracedDiConstants, inverseViewProjection) == 0);
    static_assert(offsetof(RayTracedDiConstants, previousViewProjection) == 64);
    static_assert(offsetof(RayTracedDiConstants, cameraPosition) == 128);
    static_assert(offsetof(RayTracedDiConstants, cameraForward) == 144);
    static_assert(offsetof(RayTracedDiConstants, toSunWorld) == 160);
    static_assert(offsetof(RayTracedDiConstants, sunIrradiance) == 176);
    static_assert(offsetof(RayTracedDiConstants, renderSize) == 192);
    static_assert(offsetof(RayTracedDiConstants, traceParameters) == 208);
    static_assert(offsetof(RayTracedDiConstants, samplingParameters) == 224);

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
                                                                          const RayTracingSceneBindings& scene,
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
                                                  const RayTracingSceneBindings& scene,
                                                  const RayTracingSceneGraphResources& sceneResources,
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

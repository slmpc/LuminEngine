#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "render/gi/raytracing/SharcRadianceCache.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#include "render/resources/FrameGraph.hpp"

namespace lumin::render::gi {

    /** 与 `shaders/RtGi.slang` 同构的逐帧常量。 */
    struct alignas(16) RayTracedGiConstants {
        /// xyz 为 world-space 相机位置。
        glm::vec4 cameraPosition{0.0F};
        /// xyz 为 world-space 相机前向。
        glm::vec4 cameraForward{0.0F, 0.0F, -1.0F, 0.0F};
        /// xyz 为从表面指向太阳的 world-space 单位向量。
        glm::vec4 toSunWorld{0.0F, 1.0F, 0.0F, 0.0F};
        /// rgb 为预曝光太阳入射照度，w 为天空亮度尺度。
        glm::vec4 sunIrradiance{1.0F};
        /// xy 为分辨率；zw 为 current-previous 的有效 screen-UV jitter，用于从 G-buffer motion 去抖动。
        glm::vec4 renderSize{1.0F};
        /// x=minT，y=maxT，z=成功提交后的逻辑帧序号，w=NRD denoisingRange。
        glm::vec4 traceParameters{0.001F, 10000.0F, 0.0F, 500000.0F};
    };

    static_assert(std::is_standard_layout_v<RayTracedGiConstants>);
    static_assert(sizeof(RayTracedGiConstants) == 96);
    static_assert(alignof(RayTracedGiConstants) == 16);
    static_assert(offsetof(RayTracedGiConstants, cameraPosition) == 0);
    static_assert(offsetof(RayTracedGiConstants, cameraForward) == 16);
    static_assert(offsetof(RayTracedGiConstants, toSunWorld) == 32);
    static_assert(offsetof(RayTracedGiConstants, sunIrradiance) == 48);
    static_assert(offsetof(RayTracedGiConstants, renderSize) == 64);
    static_assert(offsetof(RayTracedGiConstants, traceParameters) == 80);

    /// NRD 消费的原始 ray-traced GI 信号格式。
    struct RayTracedGiSignalFormats {
        nvrhi::Format diffuseRadianceHitDistance = nvrhi::Format::RGBA16_FLOAT;
        nvrhi::Format specularRadianceHitDistance = nvrhi::Format::RGBA16_FLOAT;
        nvrhi::Format viewZ = nvrhi::Format::R32_FLOAT;
        nvrhi::Format normalRoughness = nvrhi::Format::R10G10B10A2_UNORM;
        nvrhi::Format motion = nvrhi::Format::RG16_FLOAT;

        friend constexpr bool operator==(const RayTracedGiSignalFormats&,
                                         const RayTracedGiSignalFormats&) noexcept = default;
    };

    /// 单个帧槽拥有的五类原始 GI 信号及常量 buffer。
    struct RayTracedGiSignalResources {
        nvrhi::TextureHandle diffuseRadianceHitDistance;
        nvrhi::TextureHandle specularRadianceHitDistance;
        nvrhi::TextureHandle viewZ;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle motion;
        nvrhi::BufferHandle constants;
    };

    /// ray generation 从 RT surface 读取的物理纹理。
    struct RayTracedGiFrameInputs {
        nvrhi::TextureHandle position;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle albedoMetallic;
        nvrhi::TextureHandle motion;
        /// 用于恢复主命中点完整材质并计算 Cook-Torrance BRDF。
        nvrhi::TextureHandle materialId;
    };

    /// 上述 G-buffer 在当前 FrameGraph 中已有的资源身份。
    struct RayTracedGiFrameGraphInputs {
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        FrameGraphResourceHandle motion;
        /// 与 `RayTracedGiFrameInputs::materialId` 对应的图资源身份。
        FrameGraphResourceHandle materialId;
    };

    /** RT pass 使用的当前 GPU Scene 物理 descriptor。 */
    struct RayTracedGiSceneBindings {
        gpu::GpuSceneDescriptors descriptors;
        std::span<const gpu::GpuGeometryDescriptor> geometry;
        std::span<const nvrhi::TextureHandle> baseColorTextures;
        std::span<const nvrhi::TextureHandle> normalRoughnessTextures;
        nvrhi::SamplerHandle materialSampler;
    };

    /** 必须复用 GPU Scene upload/build 阶段导入的 FrameGraph 资源身份。 */
    struct RayTracedGiSceneGraphResources {
        FrameGraphResourceHandle tlas;
        FrameGraphResourceHandle instances;
        FrameGraphResourceHandle materials;
        std::span<const FrameGraphResourceHandle> vertices;
        std::span<const FrameGraphResourceHandle> indices;
        std::span<const FrameGraphResourceHandle> baseColorTextures;
        std::span<const FrameGraphResourceHandle> normalRoughnessTextures;
        FrameGraphPassHandle readyPass;
    };

    /// `record()` 返回的信号资源身份，供后续 SHARC/NRD pass 直接复用。
    struct RayTracedGiGraphSignals {
        FrameGraphResourceHandle diffuseRadianceHitDistance;
        FrameGraphResourceHandle specularRadianceHitDistance;
        FrameGraphResourceHandle viewZ;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle motion;
        /// 完成 SHARC query 与五类原始信号写入的 RT pass。
        FrameGraphPassHandle tracePass;
    };

    struct RayTracedGiCreateInfo {
        nvrhi::IDevice* device = nullptr;
        std::filesystem::path shaderDirectory;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t maxGeometryDescriptors = 0;
        std::uint32_t maxMaterialTextureDescriptors = 0;
        /// 与 raster sky 完全相同的 descriptor set 2 layout。
        nvrhi::BindingLayoutHandle atmosphereBindingLayout;
        /// 为 `true` 时加载 SHARC closest-hit 变体并扩展 descriptor layout。
        bool enableSharc = false;
        std::span<const RayTracedGiFrameInputs> frames;
    };

    namespace detail {

        /// 构造单个 NRD 信号纹理描述；返回值同时支持 SRV 与 UAV。
        [[nodiscard]] nvrhi::TextureDesc makeRayTracedGiSignalTextureDesc(std::uint32_t width, std::uint32_t height,
                                                                          nvrhi::Format format, const char* debugName);

        /// 构造与 `RtGi.slang` descriptor set 0 精确同构的 binding layout。
        [[nodiscard]] nvrhi::BindingLayoutDesc
        makeRayTracedGiBindingLayoutDesc(std::uint32_t maxGeometryDescriptors, bool enableSharc = false,
                                         std::uint32_t maxMaterialTextureDescriptors = 1);

        /// 校验并生成当前场景版本的 binding set。
        [[nodiscard]] nvrhi::BindingSetDesc makeRayTracedGiBindingSetDesc(const RayTracedGiFrameInputs& inputs,
                                                                          const RayTracedGiSignalResources& signals,
                                                                          const RayTracedGiSceneBindings& scene,
                                                                          std::uint32_t maxGeometryDescriptors,
                                                                          std::uint32_t maxMaterialTextureDescriptors,
                                                                          const SharcGraphRecord* sharc = nullptr);

        template <typename CommandList>
        void recordRayTracedGiDispatch(CommandList& commandList, const nvrhi::rt::State& state, std::uint32_t width,
                                       std::uint32_t height) {
            commandList.setRayTracingState(state);
            commandList.dispatchRays(nvrhi::rt::DispatchRaysArguments().setDimensions(width, height));
        }

    } // namespace detail

    /**
     * 创建原始 RT GI pipeline/SBT，并为每个帧槽输出 NRD 输入信号。
     *
     * 本类型不推进任何时序历史。调用方只能在 `beginFrame()` 已等待对应帧槽 fence 后调用 `record()`；
     * GPU Scene 的候选物理版本会被 binding set 与 pass lambda 持有到命令提交完成。
     */
    class RayTracedGiPass final {
    public:
        explicit RayTracedGiPass(const RayTracedGiCreateInfo& createInfo);
        ~RayTracedGiPass();

        RayTracedGiPass(const RayTracedGiPass&) = delete;
        RayTracedGiPass& operator=(const RayTracedGiPass&) = delete;

        /** 录制一帧 ray dispatch，并返回供后续 denoiser 使用的同一组图资源。 */
        [[nodiscard]] RayTracedGiGraphSignals record(FrameGraph& frameGraph, std::uint32_t frameIndex,
                                                     bool frameSlotFenceWaited, const RayTracedGiConstants& constants,
                                                     const RayTracedGiFrameGraphInputs& inputs,
                                                     const RayTracedGiSceneBindings& scene,
                                                     const RayTracedGiSceneGraphResources& sceneResources,
                                                     const RayTracingEnvironmentBindings& environment,
                                                     const RayTracingEnvironmentGraphResources& environmentResources,
                                                     const SharcGraphRecord* sharc = nullptr);

        /// 返回帧槽拥有的物理信号纹理。
        [[nodiscard]] const RayTracedGiSignalResources& signals(std::uint32_t frameIndex) const;
        [[nodiscard]] RayTracedGiSignalFormats formats() const noexcept;

        /** queue submit 成功后发布当前帧槽信号的跨帧初始状态。 */
        void commitSubmittedFrame();

        /** 录制、FrameGraph 执行或 queue submit 失败时放弃候选状态。 */
        void discardPendingFrame() noexcept;

        [[nodiscard]] bool hasPendingFrame() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

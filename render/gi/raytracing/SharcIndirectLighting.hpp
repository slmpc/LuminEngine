#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "render/gi/raytracing/SharcRadianceCache.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#include "render/resources/FrameGraph.hpp"

namespace lumin::render {
    class ShaderLibrary;
}

namespace lumin::render::gi {

    /** 与 `shaders/SharcIndirectLighting.slang` 精确同构的逐帧常量。 */
    struct alignas(16) SharcIndirectLightingConstants {
        /// xyz 为 world-space 相机位置。
        glm::vec4 cameraPosition{0.0F};
        /// xyz 为 world-space 相机前向；w 固定为 0。
        glm::vec4 cameraForward{0.0F, 0.0F, -1.0F, 0.0F};
        /// xy 为输出尺寸，zw 为 current jitter - previous jitter 的 UV 偏移。
        glm::vec4 renderParameters{1.0F, 1.0F, 0.0F, 0.0F};
        /// x=minT，y=maxT，z=成功提交帧序号，w=NRD denoising range。
        glm::vec4 traceParameters{0.001F, 10000.0F, 0.0F, 500000.0F};
        /// x=有效光源数量，yzw 保留。
        glm::uvec4 samplingParameters{1U, 0U, 0U, 0U};
    };

    static_assert(std::is_standard_layout_v<SharcIndirectLightingConstants>);
    static_assert(sizeof(SharcIndirectLightingConstants) == 80);
    static_assert(alignof(SharcIndirectLightingConstants) == 16);
    static_assert(offsetof(SharcIndirectLightingConstants, cameraPosition) == 0);
    static_assert(offsetof(SharcIndirectLightingConstants, cameraForward) == 16);
    static_assert(offsetof(SharcIndirectLightingConstants, renderParameters) == 32);
    static_assert(offsetof(SharcIndirectLightingConstants, traceParameters) == 48);
    static_assert(offsetof(SharcIndirectLightingConstants, samplingParameters) == 64);

    /** NRD 消费的 SHARC 间接光信号格式。 */
    struct SharcIndirectLightingSignalFormats {
        nvrhi::Format diffuseRadianceHitDistance = nvrhi::Format::RGBA16_FLOAT;
        nvrhi::Format specularRadianceHitDistance = nvrhi::Format::RGBA16_FLOAT;
        nvrhi::Format viewZ = nvrhi::Format::R32_FLOAT;
        nvrhi::Format normalRoughness = nvrhi::Format::R10G10B10A2_UNORM;
        nvrhi::Format motion = nvrhi::Format::RG16_FLOAT;

        friend constexpr bool operator==(const SharcIndirectLightingSignalFormats&,
                                         const SharcIndirectLightingSignalFormats&) noexcept = default;
    };

    /** SHARC 间接光 ray generation 读取的主表面物理纹理。 */
    struct SharcIndirectLightingFrameInputs {
        nvrhi::TextureHandle position;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle albedoMetallic;
        nvrhi::TextureHandle motion;
        nvrhi::TextureHandle materialId;
    };

    /** 单个帧槽拥有的 NRD 输入信号和常量。 */
    struct SharcIndirectLightingFrameResources {
        nvrhi::TextureHandle diffuseRadianceHitDistance;
        nvrhi::TextureHandle specularRadianceHitDistance;
        nvrhi::TextureHandle viewZ;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle motion;
        nvrhi::BufferHandle constants;
    };

    /** 主表面纹理在当前 FrameGraph 中已有的身份。 */
    struct SharcIndirectLightingFrameGraphInputs {
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        FrameGraphResourceHandle motion;
        FrameGraphResourceHandle materialId;
    };

    /** SHARC 间接光使用的当前 GPU Scene 物理 descriptor。 */
    struct SharcIndirectLightingSceneBindings {
        gpu::GpuSceneDescriptors descriptors;
        std::span<const gpu::GpuGeometryDescriptor> geometry;
        std::span<const nvrhi::TextureHandle> baseColorTextures;
        std::span<const nvrhi::TextureHandle> normalRoughnessTextures;
        nvrhi::SamplerHandle materialSampler;
    };

    /** 必须复用 GPU Scene upload/build 阶段导入的 FrameGraph 资源身份。 */
    struct SharcIndirectLightingSceneGraphResources {
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

    /** 一次间接光 dispatch 的输出图身份。 */
    struct SharcIndirectLightingGraphOutput {
        FrameGraphResourceHandle diffuseRadianceHitDistance;
        FrameGraphResourceHandle specularRadianceHitDistance;
        FrameGraphResourceHandle viewZ;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle motion;
        FrameGraphPassHandle tracePass;

        [[nodiscard]] bool isValid() const noexcept {
            return diffuseRadianceHitDistance.isValid() && specularRadianceHitDistance.isValid() && viewZ.isValid() &&
                   normalRoughness.isValid() && motion.isValid() && tracePass.isValid();
        }
    };

    /** 创建 SHARC 间接光 pipeline 与逐帧槽输出所需参数。 */
    struct SharcIndirectLightingCreateInfo {
        /** NvRHI 设备；生命周期必须覆盖 pass。 */
        nvrhi::IDevice* device = nullptr;
        /** Session 级 shader 缓存；生命周期必须覆盖创建过程。 */
        ShaderLibrary* shaders = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t maxGeometryDescriptors = 0;
        std::uint32_t maxMaterialTextureDescriptors = 0;
        /** 与 raster sky 完全相同的 descriptor set 2 layout。 */
        nvrhi::BindingLayoutHandle atmosphereBindingLayout;
        std::span<const SharcIndirectLightingFrameInputs> frames;
    };

    namespace detail {

        /** 构造支持 SRV/UAV 的 NRD 信号纹理。 */
        [[nodiscard]] nvrhi::TextureDesc makeSharcIndirectLightingTextureDesc(std::uint32_t width, std::uint32_t height,
                                                                              nvrhi::Format format,
                                                                              const char* debugName);

        /** 构造与 SHARC 间接光 shader descriptor set 0 精确同构的 layout。 */
        [[nodiscard]] nvrhi::BindingLayoutDesc
        makeSharcIndirectLightingBindingLayoutDesc(std::uint32_t maxGeometryDescriptors,
                                                   std::uint32_t maxMaterialTextureDescriptors = 1);

        template <typename CommandList>
        void recordSharcIndirectLightingDispatch(CommandList& commandList, const nvrhi::rt::State& state,
                                                 std::uint32_t width, std::uint32_t height) {
            commandList.setRayTracingState(state);
            commandList.dispatchRays(nvrhi::rt::DispatchRaysArguments().setDimensions(width, height));
        }

    } // namespace detail

    /**
     * 使用一条二次射线估计主表面的间接光，并在非 primary hit 查询本帧已 resolve 的 SHARC cache。
     *
     * 输出按 NRD REBLUR 契约解调并拆分 diffuse/specular。每帧槽资源只能在对应 fence 已等待后更新。
     */
    class SharcIndirectLightingPass final {
    public:
        explicit SharcIndirectLightingPass(const SharcIndirectLightingCreateInfo& createInfo);
        ~SharcIndirectLightingPass();

        SharcIndirectLightingPass(const SharcIndirectLightingPass&) = delete;
        SharcIndirectLightingPass& operator=(const SharcIndirectLightingPass&) = delete;

        /** 录制 SHARC 间接光 dispatch；`sharc.resolvePass` 是固定前置依赖。 */
        [[nodiscard]] SharcIndirectLightingGraphOutput
        record(FrameGraph& frameGraph, std::uint32_t frameIndex, bool frameSlotFenceWaited,
               const SharcIndirectLightingConstants& constants, const SharcIndirectLightingFrameGraphInputs& inputs,
               const SharcIndirectLightingSceneBindings& scene,
               const SharcIndirectLightingSceneGraphResources& sceneResources,
               const RayTracingEnvironmentBindings& environment,
               const RayTracingEnvironmentGraphResources& environmentResources, const SharcGraphRecord& sharc);

        /** SHARC 关闭时清零当前帧槽输出，并沿用同一提交事务维护资源状态。 */
        [[nodiscard]] SharcIndirectLightingGraphOutput recordClear(FrameGraph& frameGraph, std::uint32_t frameIndex,
                                                                   bool frameSlotFenceWaited);

        /** 返回指定帧槽拥有的物理输出。 */
        [[nodiscard]] const SharcIndirectLightingFrameResources& resources(std::uint32_t frameIndex) const;

        /** 返回 NRD 输入使用的稳定格式集合。 */
        [[nodiscard]] SharcIndirectLightingSignalFormats formats() const noexcept;

        /** queue submit 成功后发布当前帧槽输出的跨帧初始状态。 */
        void commitSubmittedFrame();

        /** 录制、FrameGraph 执行或 queue submit 失败时放弃候选状态。 */
        void discardPendingFrame() noexcept;

        [[nodiscard]] bool hasPendingFrame() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

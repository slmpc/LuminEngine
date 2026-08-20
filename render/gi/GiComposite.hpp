#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "render/resources/FrameGraph.hpp"
#include "render/core/FrameIdentity.hpp"
#include "render/gpu/GpuMaterial.hpp"

namespace lumin::render::gi {

    /** 与 `shaders/gi_composite.slang` 同构的逐帧常量。 */
    struct alignas(16) GiCompositeConstants {
        /// xyz 为 world-space 相机位置；w 固定为 1。
        glm::vec4 cameraPosition{0.0F, 0.0F, 0.0F, 1.0F};
        /// xy 为输出尺寸，z 为材质记录数，w 为无效材质索引。
        glm::uvec4 renderInfo{1U, 1U, 0U, 0xffffffffU};
    };

    static_assert(std::is_standard_layout_v<GiCompositeConstants>);
    static_assert(sizeof(GiCompositeConstants) == 32);
    static_assert(alignof(GiCompositeConstants) == 16);
    static_assert(offsetof(GiCompositeConstants, cameraPosition) == 0);
    static_assert(offsetof(GiCompositeConstants, renderInfo) == 16);

    /** composite 使用的物理 NvRHI 资源；资源本身仍由调用方拥有。 */
    struct GiCompositeResources {
        /// NRD `OUT_DIFF_RADIANCE_HITDIST`，只读取 rgb 辐亮度。
        nvrhi::TextureHandle diffuseRadianceHitDistance;
        /// NRD `OUT_SPEC_RADIANCE_HITDIST`，只读取 rgb 辐亮度。
        nvrhi::TextureHandle specularRadianceHitDistance;
        nvrhi::TextureHandle position;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle albedoMetallic;
        nvrhi::TextureHandle materialId;
        nvrhi::BufferHandle materials;
        /// 现有 packed GI 纹理：rgb 为间接光，a 为旧环境光可见度。
        nvrhi::TextureHandle globalIllumination;
    };

    /** `GiCompositeResources` 在当前 FrameGraph 中已经存在的同一组资源身份。 */
    struct GiCompositeGraphResources {
        FrameGraphResourceHandle diffuseRadianceHitDistance;
        FrameGraphResourceHandle specularRadianceHitDistance;
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        FrameGraphResourceHandle materialId;
        FrameGraphResourceHandle materials;
        FrameGraphResourceHandle globalIllumination;
    };

    /** 一次 GI composite 录制的帧槽、尺寸和观察点。 */
    struct GiCompositeFrameParameters {
        core::FrameSlotIndex frameSlot;
        core::RenderExtent extent;
        glm::vec3 cameraPosition{0.0F};
        /// 写入该帧槽常量和 binding set 之前，调用方必须已等待对应 fence。
        bool frameSlotFenceWaited = false;
    };

    struct GiCompositeCreateInfo {
        nvrhi::IDevice* device = nullptr;
        std::filesystem::path shaderDirectory;
        core::RenderExtent extent;
        std::uint32_t frameSlotCount = 0;
    };

    /** 向上取整后的 compute dispatch 数量。 */
    struct GiCompositeDispatchSize {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t z = 1;

        friend constexpr bool operator==(const GiCompositeDispatchSize&,
                                         const GiCompositeDispatchSize&) noexcept = default;
    };

    namespace detail {

        /// 构造每帧槽独占、CPU 可写的常量缓冲描述。
        [[nodiscard]] nvrhi::BufferDesc makeGiCompositeConstantBufferDesc();

        /// 构造与 `gi_composite.slang` descriptor set 0 精确同构的 layout。
        [[nodiscard]] nvrhi::BindingLayoutDesc makeGiCompositeBindingLayoutDesc();

        /// 校验物理资源并构造 descriptor set 0；常量缓冲位于 binding 7。
        [[nodiscard]] nvrhi::BindingSetDesc makeGiCompositeBindingSetDesc(const GiCompositeResources& resources,
                                                                          nvrhi::IBuffer* constants);

        /// 校验尺寸与资源格式，并返回结构化材质表中的记录数量。
        [[nodiscard]] std::uint32_t validateGiCompositeResources(const GiCompositeResources& resources,
                                                                 core::RenderExtent extent);

        /// 根据固定的 8x8 线程组计算覆盖整个输出的 dispatch 数量。
        [[nodiscard]] GiCompositeDispatchSize makeGiCompositeDispatchSize(core::RenderExtent extent);

        /// 创建常量内容；拒绝非有限相机坐标和空尺寸。
        [[nodiscard]] GiCompositeConstants makeGiCompositeConstants(const GiCompositeFrameParameters& parameters,
                                                                    std::uint32_t materialCount);

        /**
         * CPU 侧材质调制参考实现，供测试和抓帧诊断使用。
         *
         * diffuse/specular 输入已包含路径采样权重，但尚未乘主表面反射率。PBR 使用能量守恒漫反射权重与
         * Schlick Fresnel；Blinn-Phong 使用 base color 与显式 specular color。
         */
        [[nodiscard]] glm::vec3 modulateGiRadiance(const glm::vec3& diffuseRadiance, const glm::vec3& specularRadiance,
                                                   const glm::vec3& albedo, float metallic, const glm::vec3& normal,
                                                   const glm::vec3& toView,
                                                   const gpu::GpuMaterialData& material) noexcept;

        /** 注册 composite pass，并完整声明七个 SRV/只读资源、常量和一个 UAV。 */
        [[nodiscard]] FrameGraphPassHandle addGiCompositePass(FrameGraph& frameGraph,
                                                              const GiCompositeGraphResources& resources,
                                                              FrameGraphResourceHandle constants,
                                                              FrameGraphPassHandle dependency,
                                                              FrameGraph::ExecuteCallback execute);

        template <typename CommandList>
        void recordGiCompositeDispatch(CommandList& commandList, const nvrhi::ComputeState& state,
                                       GiCompositeDispatchSize dispatch) {
            if (dispatch.x == 0 || dispatch.y == 0 || dispatch.z != 1) {
                throw std::invalid_argument("GI composite dispatch dimensions are invalid.");
            }
            commandList.setComputeState(state);
            commandList.dispatch(dispatch.x, dispatch.y, dispatch.z);
        }

    } // namespace detail

    /**
     * 将 NRD 去噪后的未调制辐亮度写入引擎现有 packed GI 纹理。
     *
     * 有效几何写入 `float4(indirectRadiance, 0)`，从而让 deferred lighting 用真实间接光替代旧环境项；
     * 背景或无效材质写入 neutral output `{0, 0, 0, 1}`。本 pass 不拥有时序历史，也不导入调用方资源，
     * 因此必须同时传入物理 NvRHI 对象及其已有 FrameGraph 身份。
     */
    class GiCompositePass final {
    public:
        explicit GiCompositePass(const GiCompositeCreateInfo& createInfo);
        ~GiCompositePass();

        GiCompositePass(const GiCompositePass&) = delete;
        GiCompositePass& operator=(const GiCompositePass&) = delete;

        /** 录制单个 compute pass；`dependency` 可指向最后一个 NRD dispatch。 */
        [[nodiscard]] FrameGraphPassHandle record(FrameGraph& frameGraph, const GiCompositeFrameParameters& parameters,
                                                  const GiCompositeResources& resources,
                                                  const GiCompositeGraphResources& graphResources,
                                                  FrameGraphPassHandle dependency = {});

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

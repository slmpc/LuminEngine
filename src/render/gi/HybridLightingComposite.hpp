#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <glm/vec4.hpp>

#include "render/FrameGraph.hpp"
#include "render/core/FrameIdentity.hpp"
#include <nvrhi/nvrhi.h>

namespace lumin::render::gi {

    /** Hybrid lighting composite 的输出模式。 */
    enum class HybridLightingCompositeMode : std::uint32_t {
        /// RTDI direct radiance 与 NRD 解调后的间接辐亮度相加。
        DirectAndIndirect = 0,
        /// 只输出 RTDI direct radiance；indirect 输入不会影响结果。
        DirectOnly = 1,
        /// 输出 RTDI direct radiance，并乘以 SSAO packed GI 的 alpha 可见度。
        DirectWithAmbientVisibility = 2,
    };

    /** 与 `shaders/hybrid_lighting_composite.slang` 同构的固定尺寸常量。 */
    struct alignas(16) HybridLightingCompositeConstants {
        /// xy 为输出宽高；z 为 `HybridLightingCompositeMode`；w 保留，必须为零。
        glm::uvec4 renderInfo{1U, 1U, 0U, 0U};
    };

    static_assert(sizeof(HybridLightingCompositeConstants) == 16);
    static_assert(alignof(HybridLightingCompositeConstants) == 16);

    /** Hybrid 路径中 RTDI 与 NRD composite 的物理资源。 */
    struct HybridLightingCompositeResources {
        nvrhi::TextureHandle directRadiance;
        nvrhi::TextureHandle indirectRadiance;
        nvrhi::TextureHandle output;
    };

    /** 上述资源在当前 FrameGraph 中的身份。 */
    struct HybridLightingCompositeGraphResources {
        FrameGraphResourceHandle directRadiance;
        FrameGraphResourceHandle indirectRadiance;
        FrameGraphResourceHandle output;

        [[nodiscard]] bool isValid() const noexcept {
            return directRadiance.isValid() && indirectRadiance.isValid() && output.isValid();
        }
    };

    struct HybridLightingCompositeFrameParameters {
        core::FrameSlotIndex frameSlot;
        core::RenderExtent extent;
        HybridLightingCompositeMode mode = HybridLightingCompositeMode::DirectAndIndirect;
        bool frameSlotFenceWaited = false;
    };

    struct HybridLightingCompositeCreateInfo {
        nvrhi::IDevice* device = nullptr;
        std::filesystem::path shaderDirectory;
        core::RenderExtent extent;
        std::uint32_t frameSlotCount = 0;
    };

    /** 把 RT direct radiance 与 NRD 解调后的间接辐亮度相加，输出 HDR surface lighting。 */
    class HybridLightingCompositePass final {
    public:
        explicit HybridLightingCompositePass(const HybridLightingCompositeCreateInfo& createInfo);
        ~HybridLightingCompositePass();

        HybridLightingCompositePass(const HybridLightingCompositePass&) = delete;
        HybridLightingCompositePass& operator=(const HybridLightingCompositePass&) = delete;

        [[nodiscard]] FrameGraphPassHandle record(FrameGraph& frameGraph,
                                                  const HybridLightingCompositeFrameParameters& parameters,
                                                  const HybridLightingCompositeResources& resources,
                                                  const HybridLightingCompositeGraphResources& graphResources,
                                                  FrameGraphPassHandle dependency = {});

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

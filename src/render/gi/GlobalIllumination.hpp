#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <nvrhi/nvrhi.h>

#include "render/FrameGraph.hpp"

namespace lumin::render::world {
    class RenderWorldSnapshot;
}

namespace lumin::render {
    struct GraphicsPipelineDesc;
}

namespace lumin::render::gi {

    inline constexpr std::uint32_t indirectRadianceFirstChannel = 0;
    inline constexpr std::uint32_t ambientVisibilityChannel = 3;
    inline constexpr std::array<float, 4> neutralOutput = {0.0f, 0.0f, 0.0f, 1.0f};

    struct BackendInfo {
        std::string_view name;
        bool temporal = false;
        bool hardwareRayTracing = false;
    };

    struct RenderExtent {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct FrameResources {
        nvrhi::TextureHandle position;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle albedoMetallic;
        nvrhi::TextureHandle motion;
        nvrhi::TextureHandle depth;
        nvrhi::BufferHandle uniformBuffer;
        nvrhi::TextureHandle output;
    };

#if defined(LUMIN_GI_TESTING)
    class SsaoCreationDriver {
    public:
        virtual ~SsaoCreationDriver() = default;

        [[nodiscard]] virtual nvrhi::BufferHandle createUniform() = 0;
        [[nodiscard]] virtual nvrhi::BindingLayoutHandle createBindingLayout(const nvrhi::BindingLayoutDesc& desc) = 0;
        [[nodiscard]] virtual nvrhi::BindingSetHandle createBindingSet(const nvrhi::BindingSetDesc& desc,
                                                                       nvrhi::IBindingLayout* layout) = 0;
        [[nodiscard]] virtual nvrhi::FramebufferHandle createFramebuffer(const nvrhi::FramebufferDesc& desc) = 0;
        [[nodiscard]] virtual nvrhi::GraphicsPipelineHandle
        createPipeline(const lumin::render::GraphicsPipelineDesc& desc) = 0;
    };

    class SsaoRecordProbe {
    public:
        virtual ~SsaoRecordProbe() = default;

        virtual void clearTextureFloat(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources,
                                       const nvrhi::Color& color) = 0;
        virtual void setGraphicsState(const nvrhi::GraphicsState& state) = 0;
        virtual void draw(const nvrhi::DrawArguments& arguments) = 0;
    };
#endif

    struct CreateInfo {
        nvrhi::IDevice* device = nullptr;
        RenderExtent extent{};
        nvrhi::Format outputFormat = nvrhi::Format::UNKNOWN;
        nvrhi::SamplerHandle sampler;
        std::span<const FrameResources> frames;
#if defined(LUMIN_GI_TESTING)
        SsaoCreationDriver* creationDriver = nullptr;
        SsaoRecordProbe* recordProbe = nullptr;
#endif
    };

    struct FrameInfo {
        const world::RenderWorldSnapshot& world;
        std::uint32_t frameIndex = 0;
        std::uint64_t frameNumber = 0;
        bool enabled = true;
        bool cameraCut = false;
        RenderExtent extent{};
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        FrameGraphResourceHandle motion;
        FrameGraphResourceHandle depth;
        FrameGraphResourceHandle output;
    };

    struct HistoryInvalidationState {
        bool cameraCut = false;
        bool topologyChanged = false;
        bool backendReenabled = false;
        bool swapchainRecreated = false;
    };

    [[nodiscard]] constexpr bool shouldInvalidateHistory(const HistoryInvalidationState& state) noexcept {
        return state.cameraCut || state.topologyChanged || state.backendReenabled || state.swapchainRecreated;
    }

    namespace detail {

        template <typename CommandList>
        void recordSsaoClear(CommandList& commandList, nvrhi::ITexture* output) {
            commandList.clearTextureFloat(
                output, nvrhi::AllSubresources,
                nvrhi::Color(neutralOutput[0], neutralOutput[1], neutralOutput[2], neutralOutput[3]));
        }

        template <typename CommandList>
        void recordSsaoFullscreen(CommandList& commandList, const nvrhi::GraphicsState& state) {
            commandList.setGraphicsState(state);
            commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
        }

    } // namespace detail

    class GlobalIlluminationBackend {
    public:
        GlobalIlluminationBackend() = default;
        virtual ~GlobalIlluminationBackend() = default;

        GlobalIlluminationBackend(const GlobalIlluminationBackend&) = delete;
        GlobalIlluminationBackend& operator=(const GlobalIlluminationBackend&) = delete;
        GlobalIlluminationBackend(GlobalIlluminationBackend&&) = delete;
        GlobalIlluminationBackend& operator=(GlobalIlluminationBackend&&) = delete;

        [[nodiscard]] virtual BackendInfo info() const noexcept = 0;
        virtual void create(const CreateInfo& createInfo) = 0;
        virtual void destroy() noexcept = 0;
        virtual void invalidateHistory() noexcept = 0;
        virtual void addPasses(FrameGraph& frameGraph, const FrameInfo& frameInfo) = 0;
    };

} // namespace lumin::render::gi

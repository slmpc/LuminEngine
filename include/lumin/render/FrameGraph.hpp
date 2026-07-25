#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    enum class FrameGraphResourceKind {
        Buffer,
        Texture
    };

    enum class FrameGraphAccess {
        Read,
        Write,
        ReadWrite
    };

    enum class FrameGraphPassType {
        Transfer,
        Compute,
        Graphics,
        Present
    };

    struct FrameGraphResourceHandle {
        static constexpr std::uint32_t invalidId = (std::numeric_limits<std::uint32_t>::max)();

        std::uint32_t id = invalidId;

        [[nodiscard]] bool isValid() const noexcept;
    };

    struct FrameGraphPassHandle {
        static constexpr std::uint32_t invalidId = (std::numeric_limits<std::uint32_t>::max)();

        std::uint32_t id = invalidId;

        [[nodiscard]] bool isValid() const noexcept;
    };

    struct FrameGraphBufferDesc {
        std::uint64_t size = 0;
        nvrhi::IBuffer* buffer = nullptr;
        nvrhi::ResourceStates initialState = nvrhi::ResourceStates::Common;
        nvrhi::ResourceStates finalState = nvrhi::ResourceStates::Unknown;
    };

    struct FrameGraphTextureDesc {
        nvrhi::ITexture* texture = nullptr;
        nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources;
        nvrhi::ResourceStates initialState = nvrhi::ResourceStates::Common;
        nvrhi::ResourceStates finalState = nvrhi::ResourceStates::Unknown;
    };

    struct FrameGraphResourceInfo {
        std::string name;
        FrameGraphResourceKind kind = FrameGraphResourceKind::Buffer;
        bool imported = false;
    };

    class FrameGraphBarrierRecorder {
    public:
        virtual ~FrameGraphBarrierRecorder() = default;

        virtual void beginTrackingTextureState(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources,
                                               nvrhi::ResourceStates state) = 0;
        virtual void beginTrackingBufferState(nvrhi::IBuffer* buffer, nvrhi::ResourceStates state) = 0;
        virtual void setTextureState(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources,
                                     nvrhi::ResourceStates state) = 0;
        virtual void setBufferState(nvrhi::IBuffer* buffer, nvrhi::ResourceStates state) = 0;
        virtual void commitBarriers() = 0;
    };

    struct FrameGraphContext {
        nvrhi::ICommandList* commandList = nullptr;
        FrameGraphBarrierRecorder* barriers = nullptr;
        std::uint32_t frameIndex = 0;
        std::ostream* log = nullptr;
    };

    class FrameGraph;

    class FrameGraphBuilder {
    public:
        void read(FrameGraphResourceHandle resource,
                  nvrhi::ResourceStates state = nvrhi::ResourceStates::ShaderResource);

        void write(FrameGraphResourceHandle resource,
                   nvrhi::ResourceStates state = nvrhi::ResourceStates::UnorderedAccess);

        void readWrite(FrameGraphResourceHandle resource,
                       nvrhi::ResourceStates state = nvrhi::ResourceStates::UnorderedAccess);

        void readTexture(FrameGraphResourceHandle resource,
                         nvrhi::ResourceStates state = nvrhi::ResourceStates::ShaderResource,
                         nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources);

        void writeTexture(FrameGraphResourceHandle resource,
                          nvrhi::ResourceStates state = nvrhi::ResourceStates::RenderTarget,
                          nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources);

    private:
        friend class FrameGraph;

        FrameGraphBuilder(FrameGraph& graph, std::uint32_t passIndex);

        FrameGraph& graph_;
        std::uint32_t passIndex_ = 0;
    };

    class FrameGraph {
        friend class FrameGraphBuilder;

    public:
        using SetupCallback = std::function<void(FrameGraphBuilder&)>;
        using ExecuteCallback = std::function<void(const FrameGraphContext&)>;

        FrameGraphResourceHandle importBuffer(std::string name, const FrameGraphBufferDesc& desc);
        FrameGraphResourceHandle createBuffer(std::string name, const FrameGraphBufferDesc& desc);
        FrameGraphResourceHandle importTexture(std::string name, const FrameGraphTextureDesc& desc);
        FrameGraphResourceHandle createTexture(std::string name, const FrameGraphTextureDesc& desc);

        FrameGraphPassHandle addPass(std::string name, FrameGraphPassType type, SetupCallback setup,
                                     ExecuteCallback execute);

        void compile();
        void execute(const FrameGraphContext& context);
        void reset();

        [[nodiscard]] bool isCompiled() const noexcept;
        [[nodiscard]] std::span<const std::uint32_t> executionOrder() const noexcept;
        [[nodiscard]] FrameGraphResourceInfo resourceInfo(FrameGraphResourceHandle resource) const;
        [[nodiscard]] const std::string& passName(FrameGraphPassHandle pass) const;

    private:
        struct ResourceNode {
            std::string name;
            FrameGraphResourceKind kind = FrameGraphResourceKind::Buffer;
            bool imported = false;
            FrameGraphBufferDesc buffer;
            FrameGraphTextureDesc texture;
        };

        struct ResourceUsage {
            FrameGraphResourceHandle resource;
            FrameGraphAccess access = FrameGraphAccess::Read;
            nvrhi::ResourceStates state = nvrhi::ResourceStates::Unknown;
            nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources;
        };

        struct PassNode {
            std::string name;
            FrameGraphPassType type = FrameGraphPassType::Graphics;
            std::vector<ResourceUsage> usages;
            ExecuteCallback execute;
        };

        FrameGraphResourceHandle addResource(std::string name, FrameGraphResourceKind kind, bool imported,
                                             const FrameGraphBufferDesc* buffer, const FrameGraphTextureDesc* texture);

        void addUsage(std::uint32_t passIndex, const ResourceUsage& usage);
        void validateResource(FrameGraphResourceHandle resource) const;
        void validatePass(FrameGraphPassHandle pass) const;

        std::vector<ResourceNode> resources_;
        std::vector<PassNode> passes_;
        std::vector<std::uint32_t> executionOrder_;
        bool compiled_ = false;
    };

} // namespace lumin::render

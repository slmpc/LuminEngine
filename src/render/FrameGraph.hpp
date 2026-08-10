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
        Texture,
        AccelerationStructure,
    };

    enum class FrameGraphAccess {
        Read,
        Write,
        ReadWrite
    };

    enum class FrameGraphPassType {
        Transfer,
        Compute,
        /** @brief 执行硬件光线追踪调度的渲染通道。 */
        RayTracing,
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

    /** 描述由 FrameGraph 跟踪状态的外部光线追踪加速结构。 */
    struct FrameGraphAccelerationStructureDesc {
        /// 由后端创建并拥有的 BLAS 或 TLAS；FrameGraph 不延长其生命周期。
        nvrhi::rt::IAccelStruct* accelerationStructure = nullptr;
        /// 当前命令列表开始录制时的已知状态。
        nvrhi::ResourceStates initialState = nvrhi::ResourceStates::Common;
        /// 图执行完成后的目标状态；`Unknown` 表示不额外转换。
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
        virtual void setAccelerationStructureState(nvrhi::rt::IAccelStruct* accelerationStructure,
                                                   nvrhi::ResourceStates state) = 0;
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

        /// 声明 ray tracing pass 对 BLAS/TLAS 的只读访问。
        void readAccelerationStructure(FrameGraphResourceHandle resource,
                                       nvrhi::ResourceStates state = nvrhi::ResourceStates::AccelStructRead);

        /// 声明 acceleration-structure build/update pass 对 BLAS/TLAS 的写访问。
        void writeAccelerationStructure(FrameGraphResourceHandle resource,
                                        nvrhi::ResourceStates state = nvrhi::ResourceStates::AccelStructWrite);

        /**
         * @brief 声明当前通道必须在指定通道完成后执行。
         *
         * 依赖通道必须已经注册，并且注册顺序早于当前通道。重复声明同一个依赖不会产生重复的拓扑边。
         *
         * @param dependency 当前通道依赖的较早通道句柄。
         * @throws std::out_of_range 当句柄无效时抛出。
         * @throws std::invalid_argument 当依赖指向当前通道或尚未注册的后续通道时抛出。
         */
        void dependsOn(FrameGraphPassHandle dependency);

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
        FrameGraphResourceHandle importAccelerationStructure(
            std::string name, const FrameGraphAccelerationStructureDesc& desc);

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
            FrameGraphAccelerationStructureDesc accelerationStructure;
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
            std::vector<std::uint32_t> dependencies;
            ExecuteCallback execute;
        };

        FrameGraphResourceHandle addResource(std::string name, FrameGraphResourceKind kind, bool imported,
                                             const FrameGraphBufferDesc* buffer, const FrameGraphTextureDesc* texture,
                                             const FrameGraphAccelerationStructureDesc* accelerationStructure);

        void addUsage(std::uint32_t passIndex, const ResourceUsage& usage);
        void addPassDependency(std::uint32_t passIndex, FrameGraphPassHandle dependency);
        void validateResource(FrameGraphResourceHandle resource) const;
        void validatePass(FrameGraphPassHandle pass) const;

        std::vector<ResourceNode> resources_;
        std::vector<PassNode> passes_;
        std::vector<std::uint32_t> executionOrder_;
        bool compiled_ = false;
    };

} // namespace lumin::render

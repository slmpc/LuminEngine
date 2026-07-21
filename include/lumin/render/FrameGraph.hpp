#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

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
        static constexpr std::uint32_t invalidId = std::numeric_limits<std::uint32_t>::max();

        std::uint32_t id = invalidId;

        [[nodiscard]] bool isValid() const noexcept;
    };

    struct FrameGraphPassHandle {
        static constexpr std::uint32_t invalidId = std::numeric_limits<std::uint32_t>::max();

        std::uint32_t id = invalidId;

        [[nodiscard]] bool isValid() const noexcept;
    };

    struct FrameGraphBufferDesc {
        std::uint64_t size = 0;
        VkBufferUsageFlags usage = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
    };

    struct FrameGraphTextureDesc {
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        std::uint32_t depth = 1;
        std::uint32_t mipLevels = 1;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        VkImage image = VK_NULL_HANDLE;
        VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct FrameGraphResourceInfo {
        std::string name;
        FrameGraphResourceKind kind = FrameGraphResourceKind::Buffer;
        bool imported = false;
    };

    struct FrameGraphContext {
        VkDevice device = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        std::uint32_t frameIndex = 0;
        std::ostream* log = nullptr;
    };

    class FrameGraph;

    class FrameGraphBuilder {
    public:
        void read(FrameGraphResourceHandle resource, VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VkAccessFlags access = VK_ACCESS_MEMORY_READ_BIT);

        void write(FrameGraphResourceHandle resource, VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VkAccessFlags access = VK_ACCESS_MEMORY_WRITE_BIT);

        void readWrite(FrameGraphResourceHandle resource,
                       VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VkAccessFlags access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

        // 声明 texture 在当前 pass 中需要的 layout；实际 barrier 统一由 FrameGraph::execute 录制。
        void readTexture(FrameGraphResourceHandle resource, VkImageLayout layout, VkPipelineStageFlags stages,
                         VkAccessFlags access, VkImageAspectFlags aspectMask = 0);

        // 写入 color/depth 等 attachment 时只描述目标状态，不在业务渲染代码里手动发 barrier。
        void writeTexture(FrameGraphResourceHandle resource, VkImageLayout layout, VkPipelineStageFlags stages,
                          VkAccessFlags access, VkImageAspectFlags aspectMask = 0);

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
            VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkAccessFlags vkAccess = VK_ACCESS_MEMORY_READ_BIT;
            VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageAspectFlags aspectMask = 0;
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

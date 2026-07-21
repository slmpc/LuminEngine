#include "lumin/render/FrameGraph.hpp"

#include <algorithm>
#include <deque>
#include <ostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lumin::render {
    namespace {

        bool isReadAccess(FrameGraphAccess access) {
            return access == FrameGraphAccess::Read || access == FrameGraphAccess::ReadWrite;
        }

        bool isWriteAccess(FrameGraphAccess access) {
            return access == FrameGraphAccess::Write || access == FrameGraphAccess::ReadWrite;
        }

        std::uint64_t edgeKey(std::uint32_t from, std::uint32_t to) {
            return (static_cast<std::uint64_t>(from) << 32U) | static_cast<std::uint64_t>(to);
        }

        VkPipelineStageFlags fallbackStage(VkPipelineStageFlags stage) {
            return stage == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : stage;
        }

        struct RuntimeResourceState {
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags access = 0;
            FrameGraphAccess graphAccess = FrameGraphAccess::Read;
            bool hasUsage = false;
        };

        bool needsMemoryDependency(const RuntimeResourceState& state, FrameGraphAccess access) {
            return state.hasUsage && (isWriteAccess(state.graphAccess) || isWriteAccess(access));
        }

        void updateRuntimeState(RuntimeResourceState& state, VkPipelineStageFlags stage, VkAccessFlags access,
                                FrameGraphAccess graphAccess, VkImageLayout layout) {
            state.layout = layout;
            state.stage = fallbackStage(stage);
            state.access = access;
            state.graphAccess = graphAccess;
            state.hasUsage = true;
        }

    } // namespace

    bool FrameGraphResourceHandle::isValid() const noexcept {
        return id != invalidId;
    }

    bool FrameGraphPassHandle::isValid() const noexcept {
        return id != invalidId;
    }

    FrameGraphBuilder::FrameGraphBuilder(FrameGraph& graph, std::uint32_t passIndex)
        : graph_(graph), passIndex_(passIndex) {
    }

    void FrameGraphBuilder::read(FrameGraphResourceHandle resource, VkPipelineStageFlags stages, VkAccessFlags access) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Read, stages, access});
    }

    void FrameGraphBuilder::write(FrameGraphResourceHandle resource, VkPipelineStageFlags stages,
                                  VkAccessFlags access) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Write, stages, access});
    }

    void FrameGraphBuilder::readWrite(FrameGraphResourceHandle resource, VkPipelineStageFlags stages,
                                      VkAccessFlags access) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::ReadWrite, stages, access});
    }

    void FrameGraphBuilder::readTexture(FrameGraphResourceHandle resource, VkImageLayout layout,
                                        VkPipelineStageFlags stages, VkAccessFlags access,
                                        VkImageAspectFlags aspectMask) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Read, stages, access, layout,
                                                              aspectMask});
    }

    void FrameGraphBuilder::writeTexture(FrameGraphResourceHandle resource, VkImageLayout layout,
                                         VkPipelineStageFlags stages, VkAccessFlags access,
                                         VkImageAspectFlags aspectMask) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Write, stages, access, layout,
                                                              aspectMask});
    }

    FrameGraphResourceHandle FrameGraph::importBuffer(std::string name, const FrameGraphBufferDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Buffer, true, &desc, nullptr);
    }

    FrameGraphResourceHandle FrameGraph::createBuffer(std::string name, const FrameGraphBufferDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Buffer, false, &desc, nullptr);
    }

    FrameGraphResourceHandle FrameGraph::importTexture(std::string name, const FrameGraphTextureDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Texture, true, nullptr, &desc);
    }

    FrameGraphResourceHandle FrameGraph::createTexture(std::string name, const FrameGraphTextureDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Texture, false, nullptr, &desc);
    }

    FrameGraphPassHandle FrameGraph::addPass(std::string name, FrameGraphPassType type, SetupCallback setup,
                                             ExecuteCallback execute) {
        PassNode pass;
        pass.name = std::move(name);
        pass.type = type;
        pass.execute = std::move(execute);

        const auto passIndex = static_cast<std::uint32_t>(passes_.size());
        passes_.push_back(std::move(pass));

        if (setup) {
            FrameGraphBuilder builder(*this, passIndex);
            setup(builder);
        }

        compiled_ = false;
        return FrameGraphPassHandle{passIndex};
    }

    void FrameGraph::compile() {
        const std::uint32_t passCount = static_cast<std::uint32_t>(passes_.size());
        std::vector<std::vector<std::uint32_t>> edges(passCount);
        std::vector<std::uint32_t> indegree(passCount, 0);
        std::unordered_set<std::uint64_t> uniqueEdges;

        auto addEdge = [&](std::uint32_t from, std::uint32_t to) {
            if (from == to) {
                return;
            }

            const std::uint64_t key = edgeKey(from, to);
            if (uniqueEdges.insert(key).second) {
                edges[from].push_back(to);
                ++indegree[to];
            }
        };

        struct ResourceState {
            FrameGraphPassHandle lastWriter;
            std::vector<std::uint32_t> lastReaders;
        };

        std::vector<ResourceState> resourceStates(resources_.size());

        for (std::uint32_t passIndex = 0; passIndex < passCount; ++passIndex) {
            for (const ResourceUsage& usage : passes_[passIndex].usages) {
                validateResource(usage.resource);
                ResourceState& state = resourceStates[usage.resource.id];

                if (isReadAccess(usage.access)) {
                    if (state.lastWriter.isValid()) {
                        addEdge(state.lastWriter.id, passIndex);
                    }

                    if (std::find(state.lastReaders.begin(), state.lastReaders.end(), passIndex) ==
                        state.lastReaders.end()) {
                        state.lastReaders.push_back(passIndex);
                    }
                }

                if (isWriteAccess(usage.access)) {
                    if (state.lastWriter.isValid()) {
                        addEdge(state.lastWriter.id, passIndex);
                    }

                    for (const std::uint32_t reader : state.lastReaders) {
                        addEdge(reader, passIndex);
                    }

                    state.lastWriter = FrameGraphPassHandle{passIndex};
                    state.lastReaders.clear();
                }
            }
        }

        std::deque<std::uint32_t> ready;
        for (std::uint32_t passIndex = 0; passIndex < passCount; ++passIndex) {
            if (indegree[passIndex] == 0) {
                ready.push_back(passIndex);
            }
        }

        executionOrder_.clear();
        executionOrder_.reserve(passCount);

        while (!ready.empty()) {
            const std::uint32_t passIndex = ready.front();
            ready.pop_front();
            executionOrder_.push_back(passIndex);

            for (const std::uint32_t next : edges[passIndex]) {
                --indegree[next];
                if (indegree[next] == 0) {
                    ready.push_back(next);
                }
            }
        }

        if (executionOrder_.size() != passes_.size()) {
            throw std::runtime_error("Frame graph contains a dependency cycle.");
        }

        compiled_ = true;
    }

    void FrameGraph::execute(const FrameGraphContext& context) {
        if (!compiled_) {
            compile();
        }

        std::vector<RuntimeResourceState> states(resources_.size());
        for (std::size_t i = 0; i < resources_.size(); ++i) {
            states[i].layout = resources_[i].texture.initialLayout;
        }

        auto issueTextureBarrier = [&](const ResourceNode& resource, ResourceUsage usage, RuntimeResourceState& state) {
            if (context.commandBuffer == VK_NULL_HANDLE || resource.kind != FrameGraphResourceKind::Texture ||
                resource.texture.image == VK_NULL_HANDLE || usage.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                updateRuntimeState(state, usage.stages, usage.vkAccess, usage.access, state.layout);
                return;
            }

            const bool layoutChanged = state.layout != usage.imageLayout;
            const bool memoryDependency = needsMemoryDependency(state, usage.access);
            if (!layoutChanged && !memoryDependency) {
                updateRuntimeState(state, usage.stages, usage.vkAccess, usage.access, usage.imageLayout);
                return;
            }

            // FrameGraph 统一管理 image barrier；layout 不变但存在读写 hazard 时也会插入内存依赖。
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = !state.hasUsage || state.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : state.access;
            barrier.dstAccessMask = usage.vkAccess;
            barrier.oldLayout = state.layout;
            barrier.newLayout = usage.imageLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = resource.texture.image;
            barrier.subresourceRange.aspectMask =
                usage.aspectMask != 0 ? usage.aspectMask : resource.texture.aspectMask;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = resource.texture.mipLevels;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(context.commandBuffer, fallbackStage(state.stage), fallbackStage(usage.stages), 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);

            updateRuntimeState(state, usage.stages, usage.vkAccess, usage.access, usage.imageLayout);
        };

        auto issueBufferBarrier = [&](const ResourceNode& resource, ResourceUsage usage, RuntimeResourceState& state) {
            if (context.commandBuffer == VK_NULL_HANDLE || resource.kind != FrameGraphResourceKind::Buffer ||
                resource.buffer.buffer == VK_NULL_HANDLE) {
                updateRuntimeState(state, usage.stages, usage.vkAccess, usage.access, state.layout);
                return;
            }

            if (!needsMemoryDependency(state, usage.access)) {
                updateRuntimeState(state, usage.stages, usage.vkAccess, usage.access, state.layout);
                return;
            }

            // Buffer 没有 layout，FrameGraph 只需要为读写 hazard 建立内存依赖。
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = state.access;
            barrier.dstAccessMask = usage.vkAccess;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = resource.buffer.buffer;
            barrier.offset = 0;
            barrier.size = resource.buffer.size == 0 ? VK_WHOLE_SIZE : resource.buffer.size;

            vkCmdPipelineBarrier(context.commandBuffer, fallbackStage(state.stage), fallbackStage(usage.stages), 0, 0,
                                 nullptr, 1, &barrier, 0, nullptr);

            updateRuntimeState(state, usage.stages, usage.vkAccess, usage.access, state.layout);
        };

        for (const std::uint32_t passIndex : executionOrder_) {
            const PassNode& pass = passes_[passIndex];
            if (context.log != nullptr) {
                *context.log << "[framegraph] " << pass.name << "\n";
            }

            for (const ResourceUsage& usage : pass.usages) {
                const ResourceNode& resource = resources_[usage.resource.id];
                if (resource.kind == FrameGraphResourceKind::Texture) {
                    issueTextureBarrier(resource, usage, states[usage.resource.id]);
                } else {
                    issueBufferBarrier(resource, usage, states[usage.resource.id]);
                }
            }

            if (pass.execute) {
                pass.execute(context);
            }
        }

        for (std::size_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
            const ResourceNode& resource = resources_[resourceIndex];
            if (resource.kind != FrameGraphResourceKind::Texture ||
                resource.texture.finalLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                continue;
            }

            ResourceUsage finalUsage;
            finalUsage.resource = FrameGraphResourceHandle{static_cast<std::uint32_t>(resourceIndex)};
            finalUsage.stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            finalUsage.vkAccess = 0;
            finalUsage.imageLayout = resource.texture.finalLayout;
            finalUsage.aspectMask = resource.texture.aspectMask;
            issueTextureBarrier(resource, finalUsage, states[resourceIndex]);
        }
    }

    void FrameGraph::reset() {
        resources_.clear();
        passes_.clear();
        executionOrder_.clear();
        compiled_ = false;
    }

    bool FrameGraph::isCompiled() const noexcept {
        return compiled_;
    }

    std::span<const std::uint32_t> FrameGraph::executionOrder() const noexcept {
        return executionOrder_;
    }

    FrameGraphResourceInfo FrameGraph::resourceInfo(FrameGraphResourceHandle resource) const {
        validateResource(resource);
        const ResourceNode& node = resources_[resource.id];

        FrameGraphResourceInfo info;
        info.name = node.name;
        info.kind = node.kind;
        info.imported = node.imported;
        return info;
    }

    const std::string& FrameGraph::passName(FrameGraphPassHandle pass) const {
        validatePass(pass);
        return passes_[pass.id].name;
    }

    FrameGraphResourceHandle FrameGraph::addResource(std::string name, FrameGraphResourceKind kind, bool imported,
                                                     const FrameGraphBufferDesc* buffer,
                                                     const FrameGraphTextureDesc* texture) {
        ResourceNode node;
        node.name = std::move(name);
        node.kind = kind;
        node.imported = imported;

        if (buffer != nullptr) {
            node.buffer = *buffer;
        }

        if (texture != nullptr) {
            node.texture = *texture;
        }

        const auto resourceIndex = static_cast<std::uint32_t>(resources_.size());
        resources_.push_back(std::move(node));
        compiled_ = false;
        return FrameGraphResourceHandle{resourceIndex};
    }

    void FrameGraph::addUsage(std::uint32_t passIndex, const ResourceUsage& usage) {
        validateResource(usage.resource);
        if (passIndex >= passes_.size()) {
            throw std::out_of_range("Invalid frame graph pass handle.");
        }

        passes_[passIndex].usages.push_back(usage);
        compiled_ = false;
    }

    void FrameGraph::validateResource(FrameGraphResourceHandle resource) const {
        if (!resource.isValid() || resource.id >= resources_.size()) {
            throw std::out_of_range("Invalid frame graph resource handle.");
        }
    }

    void FrameGraph::validatePass(FrameGraphPassHandle pass) const {
        if (!pass.isValid() || pass.id >= passes_.size()) {
            throw std::out_of_range("Invalid frame graph pass handle.");
        }
    }

} // namespace lumin::render

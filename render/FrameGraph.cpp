#include "render/FrameGraph.hpp"

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

        struct RuntimeResourceState {
            nvrhi::ResourceStates state = nvrhi::ResourceStates::Unknown;
            FrameGraphAccess graphAccess = FrameGraphAccess::Read;
            bool hasUsage = false;
        };

        bool needsMemoryDependency(const RuntimeResourceState& state, FrameGraphAccess access) {
            return state.hasUsage && (isWriteAccess(state.graphAccess) || isWriteAccess(access));
        }

        class NvrhiBarrierRecorder final : public FrameGraphBarrierRecorder {
        public:
            explicit NvrhiBarrierRecorder(nvrhi::ICommandList* commandList)
                : commandList_(commandList) {
            }

            void beginTrackingTextureState(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources,
                                           nvrhi::ResourceStates state) override {
                commandList_->beginTrackingTextureState(texture, subresources, state);
            }

            void beginTrackingBufferState(nvrhi::IBuffer* buffer, nvrhi::ResourceStates state) override {
                commandList_->beginTrackingBufferState(buffer, state);
            }

            void setTextureState(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources,
                                 nvrhi::ResourceStates state) override {
                commandList_->setTextureState(texture, subresources, state);
            }

            void setBufferState(nvrhi::IBuffer* buffer, nvrhi::ResourceStates state) override {
                commandList_->setBufferState(buffer, state);
            }

            void setAccelerationStructureState(nvrhi::rt::IAccelStruct* accelerationStructure,
                                               nvrhi::ResourceStates state) override {
                commandList_->setAccelStructState(accelerationStructure, state);
            }

            void commitBarriers() override {
                commandList_->commitBarriers();
            }

        private:
            nvrhi::ICommandList* commandList_ = nullptr;
        };

        void updateRuntimeState(RuntimeResourceState& runtimeState, nvrhi::ResourceStates state,
                                FrameGraphAccess graphAccess) {
            runtimeState.state = state;
            runtimeState.graphAccess = graphAccess;
            runtimeState.hasUsage = true;
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

    void FrameGraphBuilder::read(FrameGraphResourceHandle resource, nvrhi::ResourceStates state) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Read, state});
    }

    void FrameGraphBuilder::write(FrameGraphResourceHandle resource, nvrhi::ResourceStates state) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Write, state});
    }

    void FrameGraphBuilder::readWrite(FrameGraphResourceHandle resource, nvrhi::ResourceStates state) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::ReadWrite, state});
    }

    void FrameGraphBuilder::readTexture(FrameGraphResourceHandle resource, nvrhi::ResourceStates state,
                                        nvrhi::TextureSubresourceSet subresources) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Read, state, subresources});
    }

    void FrameGraphBuilder::writeTexture(FrameGraphResourceHandle resource, nvrhi::ResourceStates state,
                                         nvrhi::TextureSubresourceSet subresources) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Write, state, subresources});
    }

    void FrameGraphBuilder::readAccelerationStructure(FrameGraphResourceHandle resource,
                                                       nvrhi::ResourceStates state) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Read, state});
    }

    void FrameGraphBuilder::writeAccelerationStructure(FrameGraphResourceHandle resource,
                                                        nvrhi::ResourceStates state) {
        graph_.addUsage(passIndex_, FrameGraph::ResourceUsage{resource, FrameGraphAccess::Write, state});
    }

    void FrameGraphBuilder::dependsOn(FrameGraphPassHandle dependency) {
        graph_.addPassDependency(passIndex_, dependency);
    }

    FrameGraphResourceHandle FrameGraph::importBuffer(std::string name, const FrameGraphBufferDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Buffer, true, &desc, nullptr, nullptr);
    }

    FrameGraphResourceHandle FrameGraph::createBuffer(std::string name, const FrameGraphBufferDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Buffer, false, &desc, nullptr, nullptr);
    }

    FrameGraphResourceHandle FrameGraph::importTexture(std::string name, const FrameGraphTextureDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Texture, true, nullptr, &desc, nullptr);
    }

    FrameGraphResourceHandle FrameGraph::createTexture(std::string name, const FrameGraphTextureDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::Texture, false, nullptr, &desc, nullptr);
    }

    FrameGraphResourceHandle FrameGraph::importAccelerationStructure(
        std::string name, const FrameGraphAccelerationStructureDesc& desc) {
        return addResource(std::move(name), FrameGraphResourceKind::AccelerationStructure, true, nullptr, nullptr,
                           &desc);
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
            for (const std::uint32_t dependency : passes_[passIndex].dependencies) {
                addEdge(dependency, passIndex);
            }

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

        NvrhiBarrierRecorder commandListBarriers(context.commandList);
        FrameGraphBarrierRecorder* barriers = context.barriers;
        if (barriers == nullptr && context.commandList != nullptr) {
            barriers = &commandListBarriers;
        }

        std::vector<RuntimeResourceState> states(resources_.size());
        for (std::size_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
            const ResourceNode& resource = resources_[resourceIndex];
            if (resource.kind == FrameGraphResourceKind::Texture) {
                states[resourceIndex].state = resource.texture.initialState;
                if (barriers != nullptr && resource.texture.texture != nullptr) {
                    barriers->beginTrackingTextureState(resource.texture.texture, resource.texture.subresources,
                                                        resource.texture.initialState);
                }
            } else if (resource.kind == FrameGraphResourceKind::Buffer) {
                states[resourceIndex].state = resource.buffer.initialState;
                if (barriers != nullptr && resource.buffer.buffer != nullptr) {
                    barriers->beginTrackingBufferState(resource.buffer.buffer, resource.buffer.initialState);
                }
            } else {
                states[resourceIndex].state = resource.accelerationStructure.initialState;
                if (barriers != nullptr && resource.accelerationStructure.accelerationStructure != nullptr) {
                    barriers->setAccelerationStructureState(resource.accelerationStructure.accelerationStructure,
                                                            resource.accelerationStructure.initialState);
                }
            }
        }

        auto issueTextureBarrier = [&](const ResourceNode& resource, const ResourceUsage& usage,
                                       RuntimeResourceState& runtimeState) {
            const bool stateChanged = runtimeState.state != usage.state;
            const bool memoryDependency = needsMemoryDependency(runtimeState, usage.access);
            if (barriers != nullptr && resource.texture.texture != nullptr && (stateChanged || memoryDependency)) {
                if (!stateChanged && usage.state != nvrhi::ResourceStates::Common) {
                    barriers->setTextureState(resource.texture.texture, usage.subresources,
                                              nvrhi::ResourceStates::CopySource);
                }
                barriers->setTextureState(resource.texture.texture, usage.subresources, usage.state);
            }
            updateRuntimeState(runtimeState, usage.state, usage.access);
        };

        auto issueBufferBarrier = [&](const ResourceNode& resource, const ResourceUsage& usage,
                                      RuntimeResourceState& runtimeState) {
            const bool stateChanged = runtimeState.state != usage.state;
            const bool memoryDependency = needsMemoryDependency(runtimeState, usage.access);
            if (barriers != nullptr && resource.buffer.buffer != nullptr && (stateChanged || memoryDependency)) {
                if (!stateChanged && usage.state != nvrhi::ResourceStates::Common) {
                    barriers->setBufferState(resource.buffer.buffer, nvrhi::ResourceStates::Common);
                }
                barriers->setBufferState(resource.buffer.buffer, usage.state);
            }
            updateRuntimeState(runtimeState, usage.state, usage.access);
        };

        auto issueAccelerationStructureBarrier = [&](const ResourceNode& resource, const ResourceUsage& usage,
                                                     RuntimeResourceState& runtimeState) {
            const bool stateChanged = runtimeState.state != usage.state;
            const bool memoryDependency = needsMemoryDependency(runtimeState, usage.access);
            nvrhi::rt::IAccelStruct* accelerationStructure =
                resource.accelerationStructure.accelerationStructure;
            if (barriers != nullptr && accelerationStructure != nullptr && (stateChanged || memoryDependency)) {
                if (!stateChanged && usage.state != nvrhi::ResourceStates::Common) {
                    barriers->setAccelerationStructureState(accelerationStructure, nvrhi::ResourceStates::Common);
                }
                barriers->setAccelerationStructureState(accelerationStructure, usage.state);
            }
            updateRuntimeState(runtimeState, usage.state, usage.access);
        };

        for (const std::uint32_t passIndex : executionOrder_) {
            const PassNode& pass = passes_[passIndex];
            if (context.log != nullptr) {
                *context.log << "[framegraph] " << pass.name << "\n";
            }

            if (context.commandList != nullptr) {
                context.commandList->beginMarker(pass.name.c_str());
            }

            for (const ResourceUsage& usage : pass.usages) {
                const ResourceNode& resource = resources_[usage.resource.id];
                if (resource.kind == FrameGraphResourceKind::Texture) {
                    issueTextureBarrier(resource, usage, states[usage.resource.id]);
                } else if (resource.kind == FrameGraphResourceKind::Buffer) {
                    issueBufferBarrier(resource, usage, states[usage.resource.id]);
                } else {
                    issueAccelerationStructureBarrier(resource, usage, states[usage.resource.id]);
                }
            }

            if (barriers != nullptr) {
                barriers->commitBarriers();
            }

            if (pass.execute) {
                pass.execute(context);
            }
            if (context.commandList != nullptr) {
                context.commandList->endMarker();
            }
        }

        bool hasFinalBarrier = false;
        for (std::size_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
            const ResourceNode& resource = resources_[resourceIndex];
            ResourceUsage finalUsage;
            finalUsage.resource = FrameGraphResourceHandle{static_cast<std::uint32_t>(resourceIndex)};
            if (resource.kind == FrameGraphResourceKind::Texture &&
                resource.texture.finalState != nvrhi::ResourceStates::Unknown) {
                finalUsage.state = resource.texture.finalState;
                finalUsage.subresources = resource.texture.subresources;
                issueTextureBarrier(resource, finalUsage, states[resourceIndex]);
                hasFinalBarrier = true;
            } else if (resource.kind == FrameGraphResourceKind::Buffer &&
                       resource.buffer.finalState != nvrhi::ResourceStates::Unknown) {
                finalUsage.state = resource.buffer.finalState;
                issueBufferBarrier(resource, finalUsage, states[resourceIndex]);
                hasFinalBarrier = true;
            } else if (resource.kind == FrameGraphResourceKind::AccelerationStructure &&
                       resource.accelerationStructure.finalState != nvrhi::ResourceStates::Unknown) {
                finalUsage.state = resource.accelerationStructure.finalState;
                const bool finalBarrierRequired = states[resourceIndex].state != finalUsage.state ||
                                                  needsMemoryDependency(states[resourceIndex], finalUsage.access);
                issueAccelerationStructureBarrier(resource, finalUsage, states[resourceIndex]);
                hasFinalBarrier = hasFinalBarrier || finalBarrierRequired;
            }
        }
        if (barriers != nullptr && hasFinalBarrier) {
            barriers->commitBarriers();
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
                                                     const FrameGraphTextureDesc* texture,
                                                     const FrameGraphAccelerationStructureDesc* accelerationStructure) {
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

        if (accelerationStructure != nullptr) {
            node.accelerationStructure = *accelerationStructure;
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

    void FrameGraph::addPassDependency(std::uint32_t passIndex, FrameGraphPassHandle dependency) {
        if (passIndex >= passes_.size()) {
            throw std::out_of_range("Invalid frame graph pass handle.");
        }
        if (!dependency.isValid()) {
            throw std::out_of_range("Invalid frame graph pass dependency handle.");
        }
        if (dependency.id == passIndex) {
            throw std::invalid_argument("A frame graph pass cannot depend on itself.");
        }
        if (dependency.id > passIndex) {
            throw std::invalid_argument("A frame graph pass can only depend on an earlier pass.");
        }

        validatePass(dependency);
        std::vector<std::uint32_t>& dependencies = passes_[passIndex].dependencies;
        if (std::find(dependencies.begin(), dependencies.end(), dependency.id) == dependencies.end()) {
            dependencies.push_back(dependency.id);
            compiled_ = false;
        }
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

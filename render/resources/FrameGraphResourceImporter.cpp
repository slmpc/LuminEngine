#include "render/resources/FrameGraphResourceImporter.hpp"

#include <stdexcept>
#include <utility>

namespace lumin::render {
    namespace {

        [[nodiscard]] bool compatible(const FrameGraphBufferDesc& left, const FrameGraphBufferDesc& right) noexcept {
            return left.buffer == right.buffer && left.size == right.size && left.initialState == right.initialState &&
                   left.finalState == right.finalState;
        }

        [[nodiscard]] bool compatible(const FrameGraphTextureDesc& left, const FrameGraphTextureDesc& right) noexcept {
            return left.texture == right.texture && left.subresources == right.subresources &&
                   left.initialState == right.initialState && left.finalState == right.finalState;
        }

        [[nodiscard]] bool compatible(const FrameGraphAccelerationStructureDesc& left,
                                      const FrameGraphAccelerationStructureDesc& right) noexcept {
            return left.accelerationStructure == right.accelerationStructure &&
                   left.initialState == right.initialState && left.finalState == right.finalState;
        }

    } // namespace

    FrameGraphResourceImporter::FrameGraphResourceImporter(FrameGraph& graph) noexcept : graph_(&graph) {
    }

    FrameGraphResourceHandle FrameGraphResourceImporter::importBuffer(std::string name,
                                                                      const FrameGraphBufferDesc& desc) {
        if (desc.buffer == nullptr) {
            throw std::invalid_argument("Cannot import a null FrameGraph buffer.");
        }
        const auto found = buffers_.find(desc.buffer);
        if (found != buffers_.end()) {
            if (!compatible(found->second.description, desc)) {
                throw std::invalid_argument("A physical buffer was imported with incompatible FrameGraph states.");
            }
            return found->second.handle;
        }
        const FrameGraphResourceHandle handle = graph_->importBuffer(std::move(name), desc);
        buffers_.emplace(desc.buffer, Entry<FrameGraphBufferDesc>{handle, desc});
        return handle;
    }

    FrameGraphResourceHandle FrameGraphResourceImporter::importTexture(std::string name,
                                                                       const FrameGraphTextureDesc& desc) {
        if (desc.texture == nullptr) {
            throw std::invalid_argument("Cannot import a null FrameGraph texture.");
        }
        const auto found = textures_.find(desc.texture);
        if (found != textures_.end()) {
            if (!compatible(found->second.description, desc)) {
                throw std::invalid_argument("A physical texture was imported with incompatible FrameGraph states.");
            }
            return found->second.handle;
        }
        const FrameGraphResourceHandle handle = graph_->importTexture(std::move(name), desc);
        textures_.emplace(desc.texture, Entry<FrameGraphTextureDesc>{handle, desc});
        return handle;
    }

    FrameGraphResourceHandle
    FrameGraphResourceImporter::importAccelerationStructure(std::string name,
                                                            const FrameGraphAccelerationStructureDesc& desc) {
        if (desc.accelerationStructure == nullptr) {
            throw std::invalid_argument("Cannot import a null FrameGraph acceleration structure.");
        }
        const auto found = accelerationStructures_.find(desc.accelerationStructure);
        if (found != accelerationStructures_.end()) {
            if (!compatible(found->second.description, desc)) {
                throw std::invalid_argument(
                    "A physical acceleration structure was imported with incompatible FrameGraph states.");
            }
            return found->second.handle;
        }
        const FrameGraphResourceHandle handle = graph_->importAccelerationStructure(std::move(name), desc);
        accelerationStructures_.emplace(desc.accelerationStructure,
                                        Entry<FrameGraphAccelerationStructureDesc>{handle, desc});
        return handle;
    }

    std::size_t FrameGraphResourceImporter::importedResourceCount() const noexcept {
        return buffers_.size() + textures_.size() + accelerationStructures_.size();
    }

} // namespace lumin::render

#include "lumin/render/VulkanResources.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace lumin::render {
    namespace {

        [[nodiscard]] nvrhi::CommandListHandle createUploadCommandList(nvrhi::IDevice& device) {
            nvrhi::CommandListHandle commandList = device.createCommandList();
            if (!commandList) {
                throw std::runtime_error("Failed to create an NvRHI upload command list.");
            }
            commandList->setEnableAutomaticBarriers(true);
            return commandList;
        }

    } // namespace

    GpuResourceManager::GpuResourceManager(nvrhi::IDevice& device) : device_(device) {
    }

    GpuBuffer GpuResourceManager::createBuffer(const nvrhi::BufferDesc& desc) const {
        if (desc.byteSize == 0) {
            throw std::invalid_argument("GPU buffers require a non-zero size.");
        }

        nvrhi::BufferDesc createDesc = desc;
        createDesc.initialState = createDesc.cpuAccess == nvrhi::CpuAccessMode::Write ? nvrhi::ResourceStates::Common
                                                                                      : nvrhi::ResourceStates::CopyDest;
        createDesc.keepInitialState = false;
        if (desc.cpuAccess == nvrhi::CpuAccessMode::None &&
            desc.initialState != nvrhi::ResourceStates::Unknown) {
            createDesc.initialState = desc.initialState;
            createDesc.keepInitialState = desc.keepInitialState;
        }

        GpuBuffer buffer;
        buffer.buffer = device_.createBuffer(createDesc);
        if (!buffer.buffer) {
            throw std::runtime_error("Failed to create an NvRHI buffer.");
        }
        buffer.size = createDesc.byteSize;
        buffer.cpuAccess = createDesc.cpuAccess;
        buffer.initialState = createDesc.initialState;
        return buffer;
    }

    GpuBuffer GpuResourceManager::createStaticBuffer(const nvrhi::BufferDesc& desc, const void* data,
                                                     std::size_t size) const {
        if (data == nullptr || size == 0 || size > desc.byteSize) {
            throw std::invalid_argument("Static buffer upload requires non-empty in-bounds data.");
        }
        if (desc.cpuAccess != nvrhi::CpuAccessMode::None) {
            throw std::invalid_argument("Static GPU buffers cannot request CPU access.");
        }

        GpuBuffer buffer = createBuffer(desc);
        nvrhi::CommandListHandle commandList = createUploadCommandList(device_);
        commandList->open();
        commandList->writeBuffer(buffer.buffer, data, size);
        commandList->close();
        executeUpload(*commandList);
        return buffer;
    }

    void GpuResourceManager::writeBuffer(const GpuBuffer& buffer, const void* data, std::size_t size,
                                         std::uint64_t offset) const {
        if (!buffer.buffer || buffer.cpuAccess != nvrhi::CpuAccessMode::Write) {
            throw std::invalid_argument("Buffer writes require a valid CPU-writable buffer.");
        }
        if (data == nullptr || size == 0 || offset > buffer.size || size > buffer.size - offset) {
            throw std::invalid_argument("Buffer write requires non-empty in-bounds data.");
        }

        void* mapped = device_.mapBuffer(buffer.buffer, nvrhi::CpuAccessMode::Write);
        if (mapped == nullptr) {
            throw std::runtime_error("Failed to map an NvRHI buffer.");
        }
        std::memcpy(static_cast<std::byte*>(mapped) + offset, data, size);
        device_.unmapBuffer(buffer.buffer);
    }

    void GpuResourceManager::destroyBuffer(GpuBuffer& buffer) const noexcept {
        buffer.buffer = nullptr;
        buffer.size = 0;
        buffer.cpuAccess = nvrhi::CpuAccessMode::None;
        buffer.initialState = nvrhi::ResourceStates::Unknown;
    }

    GpuTexture GpuResourceManager::createTexture(const nvrhi::TextureDesc& desc) const {
        if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 || desc.arraySize == 0 ||
            desc.format == nvrhi::Format::UNKNOWN) {
            throw std::invalid_argument(
                "GPU textures require a format and non-zero dimensions, mip levels, and layers.");
        }

        nvrhi::TextureDesc createDesc = desc;
        createDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        createDesc.keepInitialState = true;

        GpuTexture texture;
        texture.texture = device_.createTexture(createDesc);
        if (!texture.texture) {
            throw std::runtime_error("Failed to create an NvRHI texture.");
        }
        texture.format = createDesc.format;
        texture.width = createDesc.width;
        texture.height = createDesc.height;
        texture.mipLevels = createDesc.mipLevels;
        texture.arrayLayers = createDesc.arraySize;
        texture.initialState = createDesc.initialState;
        return texture;
    }

    void GpuResourceManager::uploadTexture(const GpuTexture& texture, const void* pixels, std::size_t rowPitch,
                                           std::size_t bytesPerLayer) const {
        if (!texture.texture || pixels == nullptr || rowPitch == 0 || bytesPerLayer == 0 || texture.mipLevels != 1) {
            throw std::invalid_argument("Texture upload requires a valid single-mip texture and non-empty pixel data.");
        }
        if (rowPitch > std::numeric_limits<std::size_t>::max() / texture.height ||
            bytesPerLayer < rowPitch * texture.height ||
            texture.arrayLayers > std::numeric_limits<std::size_t>::max() / bytesPerLayer) {
            throw std::overflow_error("Texture upload size exceeds the host size range.");
        }

        nvrhi::CommandListHandle commandList = createUploadCommandList(device_);
        commandList->open();
        const auto* layerData = static_cast<const std::byte*>(pixels);
        for (std::uint32_t layer = 0; layer < texture.arrayLayers; ++layer) {
            commandList->writeTexture(texture.texture, layer, 0, layerData + bytesPerLayer * layer, rowPitch);
        }
        commandList->close();
        executeUpload(*commandList);
    }

    void GpuResourceManager::destroyTexture(GpuTexture& texture) const noexcept {
        texture.texture = nullptr;
        texture.format = nvrhi::Format::UNKNOWN;
        texture.width = 0;
        texture.height = 0;
        texture.mipLevels = 1;
        texture.arrayLayers = 1;
        texture.initialState = nvrhi::ResourceStates::Unknown;
    }

    void GpuResourceManager::executeUpload(nvrhi::ICommandList& commandList) const {
        nvrhi::EventQueryHandle query = device_.createEventQuery();
        if (!query) {
            throw std::runtime_error("Failed to create an NvRHI upload event query.");
        }

        nvrhi::ICommandList* commandLists[] = {&commandList};
        device_.executeCommandLists(commandLists, 1, nvrhi::CommandQueue::Graphics);
        device_.setEventQuery(query, nvrhi::CommandQueue::Graphics);
        device_.waitEventQuery(query);
    }

} // namespace lumin::render

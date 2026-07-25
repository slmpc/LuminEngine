#pragma once

#include <cstddef>
#include <cstdint>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    struct GpuBuffer {
        nvrhi::BufferHandle buffer;
        std::uint64_t size = 0;
        nvrhi::CpuAccessMode cpuAccess = nvrhi::CpuAccessMode::None;
        nvrhi::ResourceStates initialState = nvrhi::ResourceStates::Unknown;
    };

    struct GpuTexture {
        nvrhi::TextureHandle texture;
        nvrhi::Format format = nvrhi::Format::UNKNOWN;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mipLevels = 1;
        std::uint32_t arrayLayers = 1;
        nvrhi::ResourceStates initialState = nvrhi::ResourceStates::Unknown;
    };

    class GpuResourceManager {
    public:
        explicit GpuResourceManager(nvrhi::IDevice& device);

        [[nodiscard]] GpuBuffer createBuffer(const nvrhi::BufferDesc& desc) const;
        [[nodiscard]] GpuBuffer createStaticBuffer(const nvrhi::BufferDesc& desc, const void* data,
                                                   std::size_t size) const;
        void writeBuffer(const GpuBuffer& buffer, const void* data, std::size_t size, std::uint64_t offset = 0) const;
        void destroyBuffer(GpuBuffer& buffer) const noexcept;

        [[nodiscard]] GpuTexture createTexture(const nvrhi::TextureDesc& desc) const;
        void uploadTexture(const GpuTexture& texture, const void* pixels, std::size_t rowPitch,
                           std::size_t bytesPerLayer) const;
        void destroyTexture(GpuTexture& texture) const noexcept;

    private:
        void executeUpload(nvrhi::ICommandList& commandList) const;

        nvrhi::IDevice& device_;
    };

} // namespace lumin::render

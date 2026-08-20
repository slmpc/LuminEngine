#include "render/resources/VulkanResources.hpp"

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>

namespace {

    using lumin::render::GpuBuffer;
    using lumin::render::GpuResourceManager;
    using lumin::render::GpuTexture;

    static_assert(std::same_as<decltype(GpuBuffer::buffer), nvrhi::BufferHandle>);
    static_assert(std::same_as<decltype(GpuTexture::texture), nvrhi::TextureHandle>);
    static_assert(std::same_as<decltype(GpuTexture::format), nvrhi::Format>);
    static_assert(std::constructible_from<GpuResourceManager, nvrhi::IDevice&>);

    static_assert(requires(GpuResourceManager& resources, GpuBuffer& buffer, GpuTexture& texture,
                           const nvrhi::BufferDesc& bufferDesc, const nvrhi::TextureDesc& textureDesc, const void* data,
                           std::size_t size) {
        { resources.createBuffer(bufferDesc) } -> std::same_as<GpuBuffer>;
        { resources.createStaticBuffer(bufferDesc, data, size) } -> std::same_as<GpuBuffer>;
        resources.writeBuffer(buffer, data, size);
        resources.destroyBuffer(buffer);
        { resources.createTexture(textureDesc) } -> std::same_as<GpuTexture>;
        resources.uploadTexture(texture, data, size, size);
        resources.destroyTexture(texture);
    });

    void require(bool condition, const char* message) {
        if (!condition) {
            std::fputs(message, stderr);
            std::fputc('\n', stderr);
            std::exit(1);
        }
    }

    std::size_t requireAfter(const std::string& contents, const char* token, std::size_t previous) {
        const std::size_t position = contents.find(token, previous);
        require(position != std::string::npos, token);
        return position + std::string(token).size();
    }

    void verifyUploadResourceContract() {
        const std::filesystem::path sourcePath =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "render/resources/VulkanResources.cpp";
        std::ifstream source(sourcePath);
        const std::string contents{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        require(!contents.empty(), "VulkanResources.cpp must be available to the resource contract test.");

        constexpr const char* forbidden[] = {"VkBuffer", "VkImage",   "VkDeviceMemory",      "vkAllocate",
                                             "vkMap",    "vkCmdCopy", "vkCmdPipelineBarrier"};
        for (const char* token : forbidden) {
            require(contents.find(token) == std::string::npos, token);
        }

        require(contents.find("setEnableAutomaticBarriers(true)") != std::string::npos,
                "Dedicated initialization upload lists must enable automatic barriers.");
        require(contents.find("setEnableAutomaticBarriers(false)") == std::string::npos,
                "Dedicated initialization upload lists are the exact automatic-barrier exception.");
        constexpr const char* explicitStateApis[] = {"beginTracking", "setTextureState", "setBufferState",
                                                       "commitBarriers"};
        for (const char* api : explicitStateApis) {
            require(contents.find(api) == std::string::npos, api);
        }

        require(contents.find("createDesc.initialState = createDesc.cpuAccess == nvrhi::CpuAccessMode::Write") !=
                    std::string::npos,
                "CPU-writable buffers must retain their common creation state.");
        require(contents.find("desc.initialState != nvrhi::ResourceStates::Unknown") != std::string::npos,
                "Static buffers must preserve an explicitly declared post-upload state.");
        require(contents.find("createDesc.keepInitialState = desc.keepInitialState") != std::string::npos,
                "Static buffers must restore their declared state when the upload list closes.");
        require(contents.find("createDesc.initialState = nvrhi::ResourceStates::ShaderResource") !=
                    std::string::npos,
                "Uploaded textures must expose ShaderResource after initialization.");
        require(contents.find("createDesc.keepInitialState = true") != std::string::npos,
                "Uploaded textures must restore ShaderResource when their upload list closes.");
        require(contents.find("buffer.initialState = createDesc.initialState") != std::string::npos,
                "Buffers must expose their documented initial state for FrameGraph imports.");
        require(contents.find("texture.initialState = createDesc.initialState") != std::string::npos,
                "Textures must expose their documented initial state for FrameGraph imports.");

        std::size_t position = requireAfter(contents, "commandList->open();", 0);
        position = requireAfter(contents, "commandList->writeBuffer", position);
        position = requireAfter(contents, "commandList->close();", position);
        position = requireAfter(contents, "executeUpload(*commandList);", position);

        position = requireAfter(contents, "commandList->open();", position);
        position = requireAfter(contents, "commandList->writeTexture", position);
        position = requireAfter(contents, "commandList->close();", position);
        requireAfter(contents, "executeUpload(*commandList);", position);

        position = requireAfter(contents, "device_.executeCommandLists", 0);
        position = requireAfter(contents, "device_.setEventQuery", position);
        requireAfter(contents, "device_.waitEventQuery", position);

        require(contents.find("size > buffer.size - offset") != std::string::npos,
                "Buffer uploads must reject out-of-bounds writes without overflowing.");
        require(contents.find("bytesPerLayer < rowPitch * texture.height") != std::string::npos,
                "Texture uploads must reject undersized layer data.");
        require(contents.find("std::numeric_limits<std::size_t>::max()") != std::string::npos,
                "Texture uploads must reject host-size overflow.");
    }

}

int main() {
    verifyUploadResourceContract();
    std::puts("PASS: dedicated NvRHI uploads use automatic barriers and restore ShaderResource textures.");
    std::puts("PASS: FrameGraph import state metadata, upload event ordering, and boundary guards are present.");
    return 0;
}

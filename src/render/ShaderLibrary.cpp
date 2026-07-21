#include "lumin/render/ShaderLibrary.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lumin::render {
    namespace {

        std::vector<char> readBinaryFile(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::ate | std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open shader file: " + path.string());
            }

            const auto fileSize = static_cast<std::size_t>(file.tellg());
            std::vector<char> buffer(fileSize);
            file.seekg(0);
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            return buffer;
        }

    } // namespace

    ShaderLibrary::ShaderLibrary(VkDevice device, std::filesystem::path shaderDirectory)
        : device_(device), shaderDirectory_(std::move(shaderDirectory)) {
    }

    VkShaderModule ShaderLibrary::loadModule(const std::filesystem::path& fileName) const {
        const std::vector<char> shaderCode = readBinaryFile(shaderDirectory_ / fileName);

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = shaderCode.size();
        createInfo.pCode = reinterpret_cast<const std::uint32_t*>(shaderCode.data());

        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create shader module: " + fileName.string());
        }

        return shaderModule;
    }

} // namespace lumin::render

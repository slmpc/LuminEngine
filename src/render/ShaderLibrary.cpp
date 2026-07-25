#include "lumin/render/ShaderLibrary.hpp"

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
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

    ShaderLibrary::ShaderLibrary(nvrhi::IDevice& device, std::filesystem::path shaderDirectory)
        : device_(device), shaderDirectory_(std::move(shaderDirectory)) {
    }

    nvrhi::ShaderHandle ShaderLibrary::loadModule(const std::filesystem::path& fileName, nvrhi::ShaderType shaderType,
                                                  std::string_view entryPoint) const {
        const std::vector<char> shaderCode = readBinaryFile(shaderDirectory_ / fileName);

        nvrhi::ShaderDesc desc;
        desc.setShaderType(shaderType).setEntryName(std::string(entryPoint)).setDebugName(fileName.string());
        nvrhi::ShaderHandle shader = device_.createShader(desc, shaderCode.data(), shaderCode.size());
        if (!shader) {
            throw std::runtime_error("Failed to create shader: " + fileName.string());
        }

        return shader;
    }

} // namespace lumin::render

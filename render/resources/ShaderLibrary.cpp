#include "render/resources/ShaderLibrary.hpp"

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

            const std::streampos endPosition = file.tellg();
            if (endPosition <= 0) {
                throw std::runtime_error("Shader file is empty or unreadable: " + path.string());
            }
            const auto fileSize = static_cast<std::size_t>(endPosition);
            std::vector<char> buffer(fileSize);
            file.seekg(0);
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (!file) {
                throw std::runtime_error("Failed to read shader file: " + path.string());
            }
            return buffer;
        }

    } // namespace

    namespace detail {

        nvrhi::ShaderType toNvrhiShaderType(ShaderStage stage) noexcept {
            switch (stage) {
            case ShaderStage::Compute:
                return nvrhi::ShaderType::Compute;
            case ShaderStage::Vertex:
                return nvrhi::ShaderType::Vertex;
            case ShaderStage::Fragment:
                return nvrhi::ShaderType::Pixel;
            case ShaderStage::RayGeneration:
                return nvrhi::ShaderType::RayGeneration;
            case ShaderStage::Miss:
                return nvrhi::ShaderType::Miss;
            case ShaderStage::ClosestHit:
                return nvrhi::ShaderType::ClosestHit;
            }
            return nvrhi::ShaderType::None;
        }

        nvrhi::ShaderDesc makeShaderDesc(const ShaderEntryDesc& entry) {
            if (entry.id == ShaderId::Count || entry.output.empty() || entry.entryPoint.empty() ||
                entry.entryPoint.find('\0') != std::string::npos) {
                throw std::invalid_argument("Shader Catalog entry is not loadable.");
            }
            const nvrhi::ShaderType shaderType = toNvrhiShaderType(entry.stage);
            if (shaderType == nvrhi::ShaderType::None) {
                throw std::invalid_argument("Shader Catalog entry has an unsupported stage.");
            }

            nvrhi::ShaderDesc shaderDesc;
            shaderDesc.setShaderType(shaderType)
                .setEntryName(entry.entryPoint)
                .setDebugName(entry.name)
                .setHlslExtensionsUAV(-1);
            return shaderDesc;
        }

    } // namespace detail

    ShaderLibrary::ShaderLibrary(nvrhi::IDevice& device, std::filesystem::path shaderDirectory)
        : device_(device), shaderDirectory_(std::move(shaderDirectory)) {
        if (shaderDirectory_.empty()) {
            throw std::invalid_argument("Shader directory cannot be empty.");
        }
    }

    nvrhi::ShaderHandle ShaderLibrary::load(ShaderId id) {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index >= cache_.size()) {
            throw std::out_of_range("Shader ID is outside the built-in Catalog.");
        }
        if (cache_[index]) {
            return cache_[index];
        }

        const ShaderEntryDesc& entry = builtinShaderCatalog().entry(id);
        const std::vector<char> shaderCode = readBinaryFile(shaderDirectory_ / entry.output);
        nvrhi::ShaderHandle shader =
            device_.createShader(detail::makeShaderDesc(entry), shaderCode.data(), shaderCode.size());
        if (!shader) {
            throw std::runtime_error("Failed to create shader: " + entry.name);
        }
        cache_[index] = shader;
        return shader;
    }

} // namespace lumin::render

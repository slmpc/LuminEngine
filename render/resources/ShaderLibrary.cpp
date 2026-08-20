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

        [[nodiscard]] bool isSingleShaderType(nvrhi::ShaderType shaderType) noexcept {
            switch (shaderType) {
            case nvrhi::ShaderType::Compute:
            case nvrhi::ShaderType::Vertex:
            case nvrhi::ShaderType::Hull:
            case nvrhi::ShaderType::Domain:
            case nvrhi::ShaderType::Geometry:
            case nvrhi::ShaderType::Pixel:
            case nvrhi::ShaderType::Amplification:
            case nvrhi::ShaderType::Mesh:
            case nvrhi::ShaderType::RayGeneration:
            case nvrhi::ShaderType::AnyHit:
            case nvrhi::ShaderType::ClosestHit:
            case nvrhi::ShaderType::Miss:
            case nvrhi::ShaderType::Intersection:
            case nvrhi::ShaderType::Callable:
                return true;
            default:
                return false;
            }
        }

    } // namespace

    namespace detail {

        bool isRayTracingShaderType(nvrhi::ShaderType shaderType) noexcept {
            switch (shaderType) {
            case nvrhi::ShaderType::RayGeneration:
            case nvrhi::ShaderType::AnyHit:
            case nvrhi::ShaderType::ClosestHit:
            case nvrhi::ShaderType::Miss:
            case nvrhi::ShaderType::Intersection:
            case nvrhi::ShaderType::Callable:
                return true;
            default:
                return false;
            }
        }

        nvrhi::ShaderDesc makeShaderDesc(const ShaderModuleDesc& desc) {
            if (desc.fileName.empty()) {
                throw std::invalid_argument("Shader module file name cannot be empty.");
            }
            if (!isSingleShaderType(desc.shaderType)) {
                throw std::invalid_argument("Shader module must select exactly one supported shader stage.");
            }
            if (desc.entryPoint.empty() || desc.entryPoint.find('\0') != std::string::npos) {
                throw std::invalid_argument("Shader module entry point must be non-empty and cannot contain NUL.");
            }
            if (desc.hlslExtensionsUAV < -1) {
                throw std::invalid_argument("Shader HLSL extension UAV must be -1 or non-negative.");
            }

            nvrhi::ShaderDesc shaderDesc;
            shaderDesc.setShaderType(desc.shaderType)
                .setEntryName(desc.entryPoint)
                .setDebugName(desc.debugName.empty() ? desc.fileName.string() : desc.debugName)
                .setHlslExtensionsUAV(desc.hlslExtensionsUAV);
            return shaderDesc;
        }

    } // namespace detail

    ShaderLibrary::ShaderLibrary(nvrhi::IDevice& device, std::filesystem::path shaderDirectory)
        : device_(device), shaderDirectory_(std::move(shaderDirectory)) {
    }

    nvrhi::ShaderHandle ShaderLibrary::loadModule(const ShaderModuleDesc& desc) const {
        const nvrhi::ShaderDesc shaderDesc = detail::makeShaderDesc(desc);
        const std::vector<char> shaderCode = readBinaryFile(shaderDirectory_ / desc.fileName);
        nvrhi::ShaderHandle shader = device_.createShader(shaderDesc, shaderCode.data(), shaderCode.size());
        if (!shader) {
            throw std::runtime_error("Failed to create shader: " + desc.fileName.string());
        }
        return shader;
    }

    nvrhi::ShaderHandle ShaderLibrary::loadModule(const std::filesystem::path& fileName, nvrhi::ShaderType shaderType,
                                                  std::string_view entryPoint) const {
        ShaderModuleDesc desc;
        desc.fileName = fileName;
        desc.shaderType = shaderType;
        desc.entryPoint = entryPoint;
        return loadModule(desc);
    }

    nvrhi::ShaderHandle ShaderLibrary::loadComputeModule(const std::filesystem::path& fileName,
                                                         std::string_view entryPoint) const {
        return loadModule(fileName, nvrhi::ShaderType::Compute, entryPoint);
    }

    nvrhi::ShaderHandle ShaderLibrary::loadRayTracingModule(const std::filesystem::path& fileName,
                                                            nvrhi::ShaderType shaderType,
                                                            std::string_view entryPoint) const {
        if (!detail::isRayTracingShaderType(shaderType)) {
            throw std::invalid_argument("Ray tracing shader module requires a ray tracing shader stage.");
        }
        return loadModule(fileName, shaderType, entryPoint);
    }

} // namespace lumin::render

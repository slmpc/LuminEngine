#pragma once

#include <filesystem>
#include <string_view>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    class ShaderLibrary {
    public:
        ShaderLibrary(nvrhi::IDevice& device, std::filesystem::path shaderDirectory);

        [[nodiscard]] nvrhi::ShaderHandle loadModule(const std::filesystem::path& fileName,
                                                     nvrhi::ShaderType shaderType, std::string_view entryPoint) const;

    private:
        nvrhi::IDevice& device_;
        std::filesystem::path shaderDirectory_;
    };

} // namespace lumin::render

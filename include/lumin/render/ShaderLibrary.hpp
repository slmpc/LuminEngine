#pragma once

#include <filesystem>

#include <vulkan/vulkan.h>

namespace lumin::render {

    class ShaderLibrary {
    public:
        ShaderLibrary(VkDevice device, std::filesystem::path shaderDirectory);

        [[nodiscard]] VkShaderModule loadModule(const std::filesystem::path& fileName) const;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        std::filesystem::path shaderDirectory_;
    };

} // namespace lumin::render

#pragma once

#include <filesystem>

#include <vulkan/vulkan.h>

#include "lumin/render/PipelineFactory.hpp"
#include "lumin/render/ShaderLibrary.hpp"

namespace lumin::render {

    class VulkanContext;

    class PipelineManager {
    public:
        PipelineManager(VulkanContext& context, std::filesystem::path shaderDirectory);
        ~PipelineManager();

        PipelineManager(const PipelineManager&) = delete;
        PipelineManager& operator=(const PipelineManager&) = delete;

        void createPostprocess(VkDescriptorSetLayout descriptorSetLayout, VkFormat colorFormat);
        void destroy() noexcept;

        [[nodiscard]] const GraphicsPipeline& postprocess() const noexcept;

    private:
        VulkanContext& context_;
        ShaderLibrary shaders_;
        PipelineFactory factory_;
        GraphicsPipeline postprocess_;
    };

}

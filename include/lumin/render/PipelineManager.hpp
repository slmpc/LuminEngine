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

        void create(VkDescriptorSetLayout descriptorSetLayout, VkFormat lightingFormat, VkFormat swapchainFormat);
        void destroy() noexcept;

        [[nodiscard]] const GraphicsPipeline& sky() const noexcept;
        [[nodiscard]] const GraphicsPipeline& deferredLighting() const noexcept;
        [[nodiscard]] const GraphicsPipeline& taa() const noexcept;
        [[nodiscard]] const GraphicsPipeline& tonemap() const noexcept;
        [[nodiscard]] const GraphicsPipeline& postprocess() const noexcept;

    private:
        VulkanContext& context_;
        ShaderLibrary shaders_;
        PipelineFactory factory_;
        GraphicsPipeline sky_;
        GraphicsPipeline deferredLighting_;
        GraphicsPipeline taa_;
        GraphicsPipeline tonemap_;
    };

} // namespace lumin::render

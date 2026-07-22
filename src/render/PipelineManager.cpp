#include "lumin/render/PipelineManager.hpp"

#include "lumin/render/VulkanContext.hpp"

#include <array>
#include <stdexcept>

namespace lumin::render {
    PipelineManager::PipelineManager(VulkanContext& context, std::filesystem::path shaderDirectory)
        : context_(context), shaders_(context.device(), std::move(shaderDirectory)), factory_(context.device()) {
    }

    PipelineManager::~PipelineManager() {
        destroy();
    }

    void PipelineManager::createPostprocess(VkDescriptorSetLayout descriptorSetLayout, VkFormat colorFormat) {
        destroy();
        VkShaderModule vertexShader = shaders_.loadModule("postprocess.vert.spv");
        VkShaderModule fragmentShader = VK_NULL_HANDLE;
        try {
            fragmentShader = shaders_.loadModule("postprocess.frag.spv");
            const std::array<VkFormat, 1> colors = {colorFormat};
            const std::array<VkVertexInputBindingDescription, 0> bindings = {};
            const std::array<VkVertexInputAttributeDescription, 0> attributes = {};
            GraphicsPipelineDesc desc;
            desc.vertexShader = vertexShader;
            desc.fragmentShader = fragmentShader;
            desc.descriptorSetLayout = descriptorSetLayout;
            desc.colorFormats = colors;
            desc.depthFormat = VK_FORMAT_UNDEFINED;
            desc.depthTestEnable = false;
            desc.depthWriteEnable = false;
            desc.vertexBindings = bindings;
            desc.vertexAttributes = attributes;
            postprocess_ = factory_.createGraphicsPipeline(desc);
        } catch (...) {
            if (fragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(context_.device(), fragmentShader, nullptr);
            }
            vkDestroyShaderModule(context_.device(), vertexShader, nullptr);
            throw;
        }
        vkDestroyShaderModule(context_.device(), fragmentShader, nullptr);
        vkDestroyShaderModule(context_.device(), vertexShader, nullptr);
    }

    void PipelineManager::destroy() noexcept {
        factory_.destroy(postprocess_);
    }

    const GraphicsPipeline& PipelineManager::postprocess() const noexcept {
        return postprocess_;
    }

}

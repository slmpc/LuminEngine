#pragma once

#include <span>
#include <vector>

#include <vulkan/vulkan.h>

namespace lumin::render {

    struct GraphicsPipeline {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    struct GraphicsPipelineDesc {
        VkShaderModule vertexShader = VK_NULL_HANDLE;
        VkShaderModule fragmentShader = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        std::span<const VkVertexInputBindingDescription> vertexBindings;
        std::span<const VkVertexInputAttributeDescription> vertexAttributes;
    };

    class PipelineFactory {
    public:
        explicit PipelineFactory(VkDevice device);

        [[nodiscard]] GraphicsPipeline createGraphicsPipeline(const GraphicsPipelineDesc& desc) const;
        void destroy(GraphicsPipeline& pipeline) const;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
    };

} // namespace lumin::render

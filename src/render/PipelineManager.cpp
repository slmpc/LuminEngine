#include "lumin/render/PipelineManager.hpp"

#include "lumin/render/VulkanContext.hpp"

#include <array>
#include <string>

namespace lumin::render {
    PipelineManager::PipelineManager(VulkanContext& context, std::filesystem::path shaderDirectory)
        : context_(context), shaders_(context.device(), std::move(shaderDirectory)), factory_(context.device()) {
    }

    PipelineManager::~PipelineManager() {
        destroy();
    }

    void PipelineManager::create(VkDescriptorSetLayout descriptorSetLayout, VkFormat ambientOcclusionFormat,
                                 VkFormat lightingFormat, VkFormat swapchainFormat) {
        destroy();

        const std::array<VkVertexInputBindingDescription, 0> bindings{};
        const std::array<VkVertexInputAttributeDescription, 0> attributes{};
        auto createFullscreen = [&](const std::string& shaderName, VkFormat colorFormat,
                                    GraphicsPipeline& destination) {
            VkShaderModule vertexShader = VK_NULL_HANDLE;
            VkShaderModule fragmentShader = VK_NULL_HANDLE;
            try {
                vertexShader = shaders_.loadModule(shaderName + ".vert.spv");
                fragmentShader = shaders_.loadModule(shaderName + ".frag.spv");
                const std::array<VkFormat, 1> colors = {colorFormat};
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
                destination = factory_.createGraphicsPipeline(desc);
            } catch (...) {
                if (fragmentShader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(context_.device(), fragmentShader, nullptr);
                }
                if (vertexShader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(context_.device(), vertexShader, nullptr);
                }
                throw;
            }
            vkDestroyShaderModule(context_.device(), fragmentShader, nullptr);
            vkDestroyShaderModule(context_.device(), vertexShader, nullptr);
        };

        try {
            createFullscreen("ssao", ambientOcclusionFormat, ssao_);
            createFullscreen("sky", lightingFormat, sky_);
            createFullscreen("deferred", lightingFormat, deferredLighting_);
            createFullscreen("taa", lightingFormat, taa_);
            createFullscreen("postprocess", swapchainFormat, tonemap_);
        } catch (...) {
            destroy();
            throw;
        }
    }

    void PipelineManager::destroy() noexcept {
        factory_.destroy(tonemap_);
        factory_.destroy(taa_);
        factory_.destroy(deferredLighting_);
        factory_.destroy(sky_);
        factory_.destroy(ssao_);
    }

    const GraphicsPipeline& PipelineManager::ssao() const noexcept {
        return ssao_;
    }

    const GraphicsPipeline& PipelineManager::sky() const noexcept {
        return sky_;
    }

    const GraphicsPipeline& PipelineManager::deferredLighting() const noexcept {
        return deferredLighting_;
    }

    const GraphicsPipeline& PipelineManager::taa() const noexcept {
        return taa_;
    }

    const GraphicsPipeline& PipelineManager::tonemap() const noexcept {
        return tonemap_;
    }

    const GraphicsPipeline& PipelineManager::postprocess() const noexcept {
        return tonemap_;
    }

} // namespace lumin::render

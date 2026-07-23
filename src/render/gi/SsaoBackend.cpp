#include "lumin/render/gi/SsaoBackend.hpp"

#include "lumin/render/PipelineFactory.hpp"
#include "lumin/render/ShaderLibrary.hpp"
#include "lumin/render/VulkanContext.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lumin::render::gi {
    namespace {

        constexpr std::uint32_t positionBinding = 0;
        constexpr std::uint32_t normalBinding = 1;
        constexpr std::uint32_t samplerBinding = 2;
        constexpr std::uint32_t uniformBinding = 3;

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

        void recordSsao(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D extent,
                        const GraphicsPipeline& pipeline, VkDescriptorSet descriptorSet) {
            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = targetView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color =
                VkClearColorValue{{neutralOutput[0], neutralOutput[1], neutralOutput[2], neutralOutput[3]}};

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea.extent = extent;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;

            const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                      0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};

            vkCmdBeginRendering(commandBuffer, &renderingInfo);
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
                                    &descriptorSet, 0, nullptr);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(commandBuffer);
        }

        class SsaoBackend final : public GlobalIlluminationBackend {
        public:
            explicit SsaoBackend(std::filesystem::path shaderDirectory) : shaderDirectory_(std::move(shaderDirectory)) {
            }

            ~SsaoBackend() override {
                destroy();
            }

            [[nodiscard]] BackendInfo info() const noexcept override {
                return BackendInfo{"SSAO", false, false};
            }

            void create(const CreateInfo& createInfo) override {
                destroy();
                if (createInfo.extent.width == 0 || createInfo.extent.height == 0 ||
                    createInfo.outputFormat == VK_FORMAT_UNDEFINED || createInfo.sampler == VK_NULL_HANDLE ||
                    createInfo.frames.empty()) {
                    throw std::invalid_argument("SSAO backend requires complete render resources.");
                }
                for (const FrameResources& frame : createInfo.frames) {
                    if (frame.positionView == VK_NULL_HANDLE || frame.normalRoughnessView == VK_NULL_HANDLE ||
                        frame.uniformBuffer == VK_NULL_HANDLE || frame.outputImage == VK_NULL_HANDLE ||
                        frame.outputView == VK_NULL_HANDLE) {
                        throw std::invalid_argument("SSAO backend received an incomplete frame resource.");
                    }
                }

                context_ = &createInfo.context;
                frames_.assign(createInfo.frames.begin(), createInfo.frames.end());
                shaders_ = std::make_unique<ShaderLibrary>(context_->device(), shaderDirectory_);
                pipelineFactory_ = std::make_unique<PipelineFactory>(context_->device());

                try {
                    createDescriptors(createInfo.sampler);
                    createPipeline(createInfo.outputFormat);
                } catch (...) {
                    destroy();
                    throw;
                }
            }

            void destroy() noexcept override {
                if (pipelineFactory_ != nullptr) {
                    pipelineFactory_->destroy(pipeline_);
                }
                if (context_ != nullptr && descriptorPool_ != VK_NULL_HANDLE) {
                    vkDestroyDescriptorPool(context_->device(), descriptorPool_, nullptr);
                }
                if (context_ != nullptr && descriptorSetLayout_ != VK_NULL_HANDLE) {
                    vkDestroyDescriptorSetLayout(context_->device(), descriptorSetLayout_, nullptr);
                }

                descriptorPool_ = VK_NULL_HANDLE;
                descriptorSetLayout_ = VK_NULL_HANDLE;
                descriptorSets_.clear();
                frames_.clear();
                pipelineFactory_.reset();
                shaders_.reset();
                context_ = nullptr;
            }

            void invalidateHistory() noexcept override {
            }

            void addPasses(FrameGraph& frameGraph, const FrameInfo& frameInfo) override {
                if (pipeline_.pipeline == VK_NULL_HANDLE || frameInfo.frameIndex >= frames_.size() ||
                    frameInfo.frameIndex >= descriptorSets_.size()) {
                    throw std::logic_error("SSAO backend is not ready for the requested frame slot.");
                }

                const VkImageView outputView = frames_[frameInfo.frameIndex].outputView;
                const VkDescriptorSet descriptorSet = descriptorSets_[frameInfo.frameIndex];
                const GraphicsPipeline pipeline = pipeline_;
                const VkExtent2D extent = frameInfo.extent;

                frameGraph.addPass(
                    "GI: SSAO", FrameGraphPassType::Graphics,
                    [position = frameInfo.position, normal = frameInfo.normalRoughness,
                     output = frameInfo.output](FrameGraphBuilder& builder) {
                        builder.readTexture(position, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                        builder.readTexture(normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                        builder.writeTexture(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                    },
                    [outputView, descriptorSet, pipeline, extent](const FrameGraphContext& context) {
                        recordSsao(context.commandBuffer, outputView, extent, pipeline, descriptorSet);
                    });
            }

        private:
            void createDescriptors(VkSampler sampler) {
                const std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
                    VkDescriptorSetLayoutBinding{positionBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                    VkDescriptorSetLayoutBinding{normalBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                    VkDescriptorSetLayoutBinding{samplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                    VkDescriptorSetLayoutBinding{uniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                };

                VkDescriptorSetLayoutCreateInfo layoutInfo{};
                layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
                layoutInfo.pBindings = bindings.data();
                checkVk(vkCreateDescriptorSetLayout(context_->device(), &layoutInfo, nullptr, &descriptorSetLayout_),
                        "Failed to create SSAO descriptor set layout.");

                const std::uint32_t frameCount = static_cast<std::uint32_t>(frames_.size());
                const std::array<VkDescriptorPoolSize, 3> poolSizes = {
                    VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, frameCount * 2},
                    VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, frameCount},
                    VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frameCount},
                };
                VkDescriptorPoolCreateInfo poolInfo{};
                poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                poolInfo.maxSets = frameCount;
                poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
                poolInfo.pPoolSizes = poolSizes.data();
                checkVk(vkCreateDescriptorPool(context_->device(), &poolInfo, nullptr, &descriptorPool_),
                        "Failed to create SSAO descriptor pool.");

                std::vector<VkDescriptorSetLayout> layouts(frameCount, descriptorSetLayout_);
                descriptorSets_.resize(frameCount);
                VkDescriptorSetAllocateInfo allocateInfo{};
                allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocateInfo.descriptorPool = descriptorPool_;
                allocateInfo.descriptorSetCount = frameCount;
                allocateInfo.pSetLayouts = layouts.data();
                checkVk(vkAllocateDescriptorSets(context_->device(), &allocateInfo, descriptorSets_.data()),
                        "Failed to allocate SSAO descriptor sets.");

                for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                    const FrameResources& frame = frames_[frameIndex];
                    const std::array<VkDescriptorImageInfo, 2> images = {
                        VkDescriptorImageInfo{VK_NULL_HANDLE, frame.positionView,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                        VkDescriptorImageInfo{VK_NULL_HANDLE, frame.normalRoughnessView,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    };
                    const VkDescriptorImageInfo samplerInfo{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                    const VkDescriptorBufferInfo uniformInfo{frame.uniformBuffer, 0, VK_WHOLE_SIZE};

                    std::array<VkWriteDescriptorSet, 4> writes{};
                    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[0].dstSet = descriptorSets_[frameIndex];
                    writes[0].dstBinding = positionBinding;
                    writes[0].descriptorCount = 1;
                    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    writes[0].pImageInfo = &images[0];
                    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[1].dstSet = descriptorSets_[frameIndex];
                    writes[1].dstBinding = normalBinding;
                    writes[1].descriptorCount = 1;
                    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    writes[1].pImageInfo = &images[1];
                    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[2].dstSet = descriptorSets_[frameIndex];
                    writes[2].dstBinding = samplerBinding;
                    writes[2].descriptorCount = 1;
                    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                    writes[2].pImageInfo = &samplerInfo;
                    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[3].dstSet = descriptorSets_[frameIndex];
                    writes[3].dstBinding = uniformBinding;
                    writes[3].descriptorCount = 1;
                    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    writes[3].pBufferInfo = &uniformInfo;

                    vkUpdateDescriptorSets(context_->device(), static_cast<std::uint32_t>(writes.size()), writes.data(),
                                           0, nullptr);
                }
            }

            void createPipeline(VkFormat outputFormat) {
                VkShaderModule vertexShader = VK_NULL_HANDLE;
                VkShaderModule fragmentShader = VK_NULL_HANDLE;
                try {
                    vertexShader = shaders_->loadModule("ssao.vert.spv");
                    fragmentShader = shaders_->loadModule("ssao.frag.spv");
                    const std::array<VkFormat, 1> colorFormats = {outputFormat};
                    const std::array<VkVertexInputBindingDescription, 0> bindings{};
                    const std::array<VkVertexInputAttributeDescription, 0> attributes{};

                    GraphicsPipelineDesc desc;
                    desc.vertexShader = vertexShader;
                    desc.fragmentShader = fragmentShader;
                    desc.descriptorSetLayout = descriptorSetLayout_;
                    desc.colorFormats = colorFormats;
                    desc.depthFormat = VK_FORMAT_UNDEFINED;
                    desc.depthTestEnable = false;
                    desc.depthWriteEnable = false;
                    desc.vertexBindings = bindings;
                    desc.vertexAttributes = attributes;
                    pipeline_ = pipelineFactory_->createGraphicsPipeline(desc);
                } catch (...) {
                    if (fragmentShader != VK_NULL_HANDLE) {
                        vkDestroyShaderModule(context_->device(), fragmentShader, nullptr);
                    }
                    if (vertexShader != VK_NULL_HANDLE) {
                        vkDestroyShaderModule(context_->device(), vertexShader, nullptr);
                    }
                    throw;
                }

                vkDestroyShaderModule(context_->device(), fragmentShader, nullptr);
                vkDestroyShaderModule(context_->device(), vertexShader, nullptr);
            }

            std::filesystem::path shaderDirectory_;
            VulkanContext* context_ = nullptr;
            std::vector<FrameResources> frames_;
            VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
            VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
            std::vector<VkDescriptorSet> descriptorSets_;
            std::unique_ptr<ShaderLibrary> shaders_;
            std::unique_ptr<PipelineFactory> pipelineFactory_;
            GraphicsPipeline pipeline_;
        };

    } // namespace

    std::unique_ptr<GlobalIlluminationBackend> makeSsaoBackend(std::filesystem::path shaderDirectory) {
        return std::make_unique<SsaoBackend>(std::move(shaderDirectory));
    }

} // namespace lumin::render::gi

#include "lumin/render/ModelRenderer.hpp"

#include "lumin/render/PipelineFactory.hpp"
#include "lumin/render/ShaderLibrary.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/render/VulkanResources.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lumin::render {
    namespace {

        std::uint32_t checkedU32(std::size_t value, const char* message) {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(message);
            }
            return static_cast<std::uint32_t>(value);
        }

        std::int32_t checkedI32(std::size_t value, const char* message) {
            if (value > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                throw std::overflow_error(message);
            }
            return static_cast<std::int32_t>(value);
        }

        struct MeshRange {
            std::uint32_t firstIndex = 0;
            std::uint32_t indexCount = 0;
            std::int32_t vertexOffset = 0;
        };

        void checkVk(VkResult result, const char* message) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error(message);
            }
        }

        std::array<VkVertexInputAttributeDescription, 2> vertexAttributes() {
            return {
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                  static_cast<std::uint32_t>(offsetof(assets::Vertex, position))},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                  static_cast<std::uint32_t>(offsetof(assets::Vertex, normal))},
            };
        }

    }

    struct ModelRenderer::Impl {
        struct alignas(16) FrameUniforms {
            glm::mat4 viewProjection{1.0f};
        };

        VulkanContext& context;
        VulkanResourceManager resources;
        ShaderLibrary shaders;
        PipelineFactory pipelineFactory;
        ModelBatch batch;
        std::uint32_t frameCount = 0;

        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets;
        GraphicsPipeline pipeline;
        VulkanBuffer vertexBuffer;
        VulkanBuffer indexBuffer;
        VulkanBuffer indirectBuffer;
        VulkanBuffer objectBuffer;
        std::vector<VulkanBuffer> uniformBuffers;

        Impl(VulkanContext& contextValue, const scene::Level& level, std::filesystem::path shaderDirectory,
             std::span<const VkFormat> colorFormats, VkFormat depthFormat, std::uint32_t frameCountValue)
            : context(contextValue), resources(contextValue), shaders(contextValue.device(), std::move(shaderDirectory)),
              pipelineFactory(contextValue.device()), batch(ModelRenderer::buildBatch(level)),
              frameCount(frameCountValue) {
            try {
                if (batch.commands.empty()) {
                    throw std::invalid_argument("ModelRenderer requires at least one model.");
                }
                if (frameCount == 0) {
                    throw std::invalid_argument("ModelRenderer requires at least one frame slot.");
                }

                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(context.physicalDevice(), &properties);
                if (batch.commands.size() > properties.limits.maxDrawIndirectCount) {
                    throw std::length_error("Level model count exceeds maxDrawIndirectCount.");
                }

                createBuffers();
                createDescriptors();
                createPipeline(colorFormats, depthFormat);
            } catch (...) {
                destroy();
                throw;
            }
        }

        ~Impl() {
            destroy();
        }

        void createBuffers() {
            const VkMemoryPropertyFlags hostMemory =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            const VkDeviceSize vertexBytes = sizeof(assets::Vertex) * batch.vertices.size();
            const VkDeviceSize indexBytes = sizeof(std::uint32_t) * batch.indices.size();
            const VkDeviceSize indirectBytes = sizeof(VkDrawIndexedIndirectCommand) * batch.commands.size();
            const VkDeviceSize objectBytes = sizeof(ObjectData) * batch.objects.size();

            vertexBuffer = resources.createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostMemory);
            indexBuffer = resources.createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hostMemory);
            indirectBuffer = resources.createBuffer(indirectBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, hostMemory);
            objectBuffer = resources.createBuffer(objectBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory);
            resources.writeBuffer(vertexBuffer, batch.vertices.data(), vertexBytes);
            resources.writeBuffer(indexBuffer, batch.indices.data(), indexBytes);
            resources.writeBuffer(indirectBuffer, batch.commands.data(), indirectBytes);
            resources.writeBuffer(objectBuffer, batch.objects.data(), objectBytes);

            uniformBuffers.resize(frameCount);
            for (VulkanBuffer& buffer : uniformBuffers) {
                buffer = resources.createBuffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostMemory);
            }
        }

        void createDescriptors() {
            const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
                VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                             nullptr},
                VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                             nullptr},
            };
            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();
            checkVk(vkCreateDescriptorSetLayout(context.device(), &layoutInfo, nullptr, &descriptorSetLayout),
                    "Failed to create model descriptor set layout.");

            const std::array<VkDescriptorPoolSize, 2> poolSizes = {
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frameCount},
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameCount},
            };
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = frameCount;
            poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            checkVk(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr, &descriptorPool),
                    "Failed to create model descriptor pool.");

            descriptorSets.resize(frameCount);
            std::vector<VkDescriptorSetLayout> layouts(frameCount, descriptorSetLayout);
            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.descriptorPool = descriptorPool;
            allocateInfo.descriptorSetCount = frameCount;
            allocateInfo.pSetLayouts = layouts.data();
            checkVk(vkAllocateDescriptorSets(context.device(), &allocateInfo, descriptorSets.data()),
                    "Failed to allocate model descriptor sets.");

            for (std::uint32_t index = 0; index < frameCount; ++index) {
                const VkDescriptorBufferInfo uniformInfo{uniformBuffers[index].buffer, 0, sizeof(FrameUniforms)};
                const VkDescriptorBufferInfo objectInfo{objectBuffer.buffer, 0, objectBuffer.size};
                const std::array<VkWriteDescriptorSet, 2> writes = {
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets[index], 0, 0,
                                         1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniformInfo, nullptr},
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets[index], 1, 0,
                                         1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objectInfo, nullptr},
                };
                vkUpdateDescriptorSets(context.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                                       nullptr);
            }
        }

        void createPipeline(std::span<const VkFormat> colorFormats, VkFormat depthFormat) {
            VkShaderModule vertexShader = VK_NULL_HANDLE;
            VkShaderModule fragmentShader = VK_NULL_HANDLE;
            try {
                vertexShader = shaders.loadModule("gbuffer.vert.spv");
                fragmentShader = shaders.loadModule("gbuffer.frag.spv");

                const VkVertexInputBindingDescription binding{0, sizeof(assets::Vertex),
                                                              VK_VERTEX_INPUT_RATE_VERTEX};
                const std::array<VkVertexInputBindingDescription, 1> bindings = {binding};
                const auto attributes = vertexAttributes();
                GraphicsPipelineDesc desc;
                desc.vertexShader = vertexShader;
                desc.fragmentShader = fragmentShader;
                desc.descriptorSetLayout = descriptorSetLayout;
                desc.colorFormats = colorFormats;
                desc.depthFormat = depthFormat;
                desc.vertexBindings = bindings;
                desc.vertexAttributes = attributes;
                pipeline = pipelineFactory.createGraphicsPipeline(desc);
            } catch (...) {
                if (fragmentShader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(context.device(), fragmentShader, nullptr);
                }
                if (vertexShader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(context.device(), vertexShader, nullptr);
                }
                throw;
            }
            vkDestroyShaderModule(context.device(), fragmentShader, nullptr);
            vkDestroyShaderModule(context.device(), vertexShader, nullptr);
        }

        void destroy() noexcept {
            pipelineFactory.destroy(pipeline);
            if (descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(context.device(), descriptorPool, nullptr);
                descriptorPool = VK_NULL_HANDLE;
            }
            if (descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(context.device(), descriptorSetLayout, nullptr);
                descriptorSetLayout = VK_NULL_HANDLE;
            }
            for (VulkanBuffer& buffer : uniformBuffers) {
                resources.destroyBuffer(buffer);
            }
            resources.destroyBuffer(objectBuffer);
            resources.destroyBuffer(indirectBuffer);
            resources.destroyBuffer(indexBuffer);
            resources.destroyBuffer(vertexBuffer);
        }
    };

    ModelBatch ModelRenderer::buildBatch(const scene::Level& level) {
        ModelBatch batch;
        std::vector<MeshRange> meshRanges;
        meshRanges.reserve(level.meshes().size());

        for (const assets::Mesh& mesh : level.meshes()) {
            const MeshRange range{
                checkedU32(batch.indices.size(), "Packed model indices exceed Vulkan's uint32 range."),
                checkedU32(mesh.indices.size(), "A model has too many indices for an indirect draw."),
                checkedI32(batch.vertices.size(), "Packed model vertices exceed Vulkan's int32 vertex offset."),
            };
            meshRanges.push_back(range);
            batch.vertices.insert(batch.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
            batch.indices.insert(batch.indices.end(), mesh.indices.begin(), mesh.indices.end());
        }

        batch.commands.reserve(level.models().size());
        batch.objects.reserve(level.models().size());
        for (const scene::ModelInstance& model : level.models()) {
            if (!model.mesh.isValid() || model.mesh.index >= meshRanges.size()) {
                throw std::out_of_range("A level model references an invalid mesh while building the render batch.");
            }

            const MeshRange range = meshRanges[model.mesh.index];
            batch.commands.push_back(VkDrawIndexedIndirectCommand{
                .indexCount = range.indexCount,
                .instanceCount = 1,
                .firstIndex = range.firstIndex,
                .vertexOffset = range.vertexOffset,
                .firstInstance = 0,
            });
            batch.objects.push_back(ObjectData{
                .model = model.transform.matrix(),
                .albedoRoughness = glm::vec4{model.material.albedo, model.material.roughness},
            });
        }

        // 命令与对象数组严格同序，shader 可用 SV_DrawIndex 无分支地读取当前模型数据。
        if (batch.commands.size() != batch.objects.size()) {
            throw std::logic_error("Indirect command and object data counts diverged.");
        }
        return batch;
    }

    ModelRenderer::ModelRenderer(VulkanContext& context, const scene::Level& level,
                                 std::filesystem::path shaderDirectory, std::span<const VkFormat> colorFormats,
                                 VkFormat depthFormat, std::uint32_t frameCount)
        : impl_(std::make_unique<Impl>(context, level, std::move(shaderDirectory), colorFormats, depthFormat,
                                      frameCount)) {
    }

    ModelRenderer::~ModelRenderer() = default;

    void ModelRenderer::record(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, const scene::Camera& camera,
                               float aspectRatio) {
        if (frameIndex >= impl_->frameCount) {
            throw std::out_of_range("ModelRenderer frame index is out of range.");
        }

        const Impl::FrameUniforms uniforms{camera.projectionMatrix(aspectRatio) * camera.viewMatrix()};
        impl_->resources.writeBuffer(impl_->uniformBuffers[frameIndex], &uniforms, sizeof(uniforms));

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline.pipeline);
        const VkBuffer vertexBuffers[] = {impl_->vertexBuffer.buffer};
        const VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, impl_->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline.layout, 0, 1,
                                &impl_->descriptorSets[frameIndex], 0, nullptr);

        // 一次 MDI 调用提交全部模型；drawCount 不是布尔值，而是完整命令数量。
        vkCmdDrawIndexedIndirect(commandBuffer, impl_->indirectBuffer.buffer, 0,
                                 static_cast<std::uint32_t>(impl_->batch.commands.size()),
                                 sizeof(VkDrawIndexedIndirectCommand));
    }

    std::uint32_t ModelRenderer::drawCount() const noexcept {
        return static_cast<std::uint32_t>(impl_->batch.commands.size());
    }

}

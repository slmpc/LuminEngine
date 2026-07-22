#include "lumin/render/ModelRenderer.hpp"

#include "lumin/render/PipelineFactory.hpp"
#include "lumin/render/ShaderLibrary.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/render/VulkanResources.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

namespace lumin::render {
    namespace {

        constexpr std::uint32_t cascadeCount = 4;

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

    } // namespace

    struct ModelRenderer::Impl {
        struct alignas(16) FrameUniforms {
            glm::mat4 viewProjection{1.0f};
            glm::mat4 previousViewProjection{1.0f};
        };

        struct alignas(16) ShadowUniforms {
            glm::mat4 lightViewProjection{1.0f};
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
        std::vector<VkDescriptorSet> shadowDescriptorSets;
        GraphicsPipeline gbufferPipeline;
        GraphicsPipeline shadowPipeline;
        VulkanBuffer vertexBuffer;
        VulkanBuffer indexBuffer;
        VulkanBuffer indirectBuffer;
        std::vector<VulkanBuffer> objectBuffers;
        std::vector<VulkanBuffer> uniformBuffers;
        std::vector<VulkanBuffer> shadowUniformBuffers;
        std::vector<glm::mat4> previousModels;
        bool hasPreviousModels = false;

        Impl(VulkanContext& contextValue, const scene::Level& level, std::filesystem::path shaderDirectory,
             std::span<const VkFormat> colorFormats, VkFormat depthFormat, VkFormat shadowDepthFormat,
             std::uint32_t frameCountValue)
            : context(contextValue), resources(contextValue),
              shaders(contextValue.device(), std::move(shaderDirectory)), pipelineFactory(contextValue.device()),
              batch(ModelRenderer::buildBatch(level)), frameCount(frameCountValue) {
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

                previousModels.reserve(batch.objects.size());
                for (const ObjectData& object : batch.objects) {
                    previousModels.push_back(object.model);
                }
                createBuffers();
                createDescriptors();
                createPipelines(colorFormats, depthFormat, shadowDepthFormat);
            } catch (...) {
                destroy();
                throw;
            }
        }

        ~Impl() {
            destroy();
        }

        [[nodiscard]] std::uint32_t shadowIndex(std::uint32_t frameIndex, std::uint32_t cascadeIndex) const {
            return frameIndex * cascadeCount + cascadeIndex;
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
            resources.writeBuffer(vertexBuffer, batch.vertices.data(), vertexBytes);
            resources.writeBuffer(indexBuffer, batch.indices.data(), indexBytes);
            resources.writeBuffer(indirectBuffer, batch.commands.data(), indirectBytes);

            objectBuffers.resize(frameCount);
            uniformBuffers.resize(frameCount);
            for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                objectBuffers[frameIndex] =
                    resources.createBuffer(objectBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory);
                resources.writeBuffer(objectBuffers[frameIndex], batch.objects.data(), objectBytes);
                uniformBuffers[frameIndex] =
                    resources.createBuffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostMemory);
            }

            shadowUniformBuffers.resize(frameCount * cascadeCount);
            for (VulkanBuffer& buffer : shadowUniformBuffers) {
                buffer = resources.createBuffer(sizeof(ShadowUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostMemory);
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

            const std::uint32_t setCount = frameCount * (1 + cascadeCount);
            const std::array<VkDescriptorPoolSize, 2> poolSizes = {
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount},
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount},
            };
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = setCount;
            poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            checkVk(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr, &descriptorPool),
                    "Failed to create model descriptor pool.");

            std::vector<VkDescriptorSetLayout> layouts(setCount, descriptorSetLayout);
            std::vector<VkDescriptorSet> allocatedSets(setCount);
            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.descriptorPool = descriptorPool;
            allocateInfo.descriptorSetCount = setCount;
            allocateInfo.pSetLayouts = layouts.data();
            checkVk(vkAllocateDescriptorSets(context.device(), &allocateInfo, allocatedSets.data()),
                    "Failed to allocate model descriptor sets.");

            descriptorSets.assign(allocatedSets.begin(), allocatedSets.begin() + frameCount);
            shadowDescriptorSets.assign(allocatedSets.begin() + frameCount, allocatedSets.end());

            auto updateSet = [&](VkDescriptorSet set, const VulkanBuffer& uniform, const VulkanBuffer& objects,
                                 VkDeviceSize uniformSize) {
                const VkDescriptorBufferInfo uniformInfo{uniform.buffer, 0, uniformSize};
                const VkDescriptorBufferInfo objectInfo{objects.buffer, 0, objects.size};
                const std::array<VkWriteDescriptorSet, 2> writes = {
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniformInfo, nullptr},
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
                                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objectInfo, nullptr},
                };
                vkUpdateDescriptorSets(context.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                                       nullptr);
            };

            for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                updateSet(descriptorSets[frameIndex], uniformBuffers[frameIndex], objectBuffers[frameIndex],
                          sizeof(FrameUniforms));
                for (std::uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
                    const std::uint32_t index = shadowIndex(frameIndex, cascadeIndex);
                    updateSet(shadowDescriptorSets[index], shadowUniformBuffers[index], objectBuffers[frameIndex],
                              sizeof(ShadowUniforms));
                }
            }
        }

        void createPipelines(std::span<const VkFormat> colorFormats, VkFormat depthFormat, VkFormat shadowDepthFormat) {
            const VkVertexInputBindingDescription binding{0, sizeof(assets::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            const std::array<VkVertexInputBindingDescription, 1> bindings = {binding};
            const auto attributes = vertexAttributes();
            const std::array<VkVertexInputAttributeDescription, 1> shadowAttributes = {attributes[0]};

            VkShaderModule vertexShader = VK_NULL_HANDLE;
            VkShaderModule fragmentShader = VK_NULL_HANDLE;
            try {
                vertexShader = shaders.loadModule("gbuffer.vert.spv");
                fragmentShader = shaders.loadModule("gbuffer.frag.spv");
                GraphicsPipelineDesc desc;
                desc.vertexShader = vertexShader;
                desc.fragmentShader = fragmentShader;
                desc.descriptorSetLayout = descriptorSetLayout;
                desc.colorFormats = colorFormats;
                desc.depthFormat = depthFormat;
                desc.cullMode = VK_CULL_MODE_NONE;
                desc.vertexBindings = bindings;
                desc.vertexAttributes = attributes;
                gbufferPipeline = pipelineFactory.createGraphicsPipeline(desc);
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

            vertexShader = VK_NULL_HANDLE;
            try {
                vertexShader = shaders.loadModule("shadow.vert.spv");
                const std::array<VkFormat, 0> noColors{};
                GraphicsPipelineDesc desc;
                desc.vertexShader = vertexShader;
                desc.fragmentShader = VK_NULL_HANDLE;
                desc.descriptorSetLayout = descriptorSetLayout;
                desc.colorFormats = noColors;
                desc.depthFormat = shadowDepthFormat;
                desc.depthTestEnable = true;
                desc.depthWriteEnable = true;
                desc.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                desc.cullMode = VK_CULL_MODE_NONE;
                desc.depthBiasEnable = true;
                desc.depthBiasConstantFactor = 1.25f;
                desc.depthBiasSlopeFactor = 1.75f;
                desc.vertexBindings = bindings;
                desc.vertexAttributes = shadowAttributes;
                shadowPipeline = pipelineFactory.createGraphicsPipeline(desc);
            } catch (...) {
                if (vertexShader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(context.device(), vertexShader, nullptr);
                }
                throw;
            }
            vkDestroyShaderModule(context.device(), vertexShader, nullptr);
        }

        void bindGeometryAndDraw(VkCommandBuffer commandBuffer, const GraphicsPipeline& pipeline,
                                 VkDescriptorSet descriptorSet) const {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
            const VkBuffer vertexBuffers[] = {vertexBuffer.buffer};
            const VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
                                    &descriptorSet, 0, nullptr);
            vkCmdDrawIndexedIndirect(commandBuffer, indirectBuffer.buffer, 0,
                                     static_cast<std::uint32_t>(batch.commands.size()),
                                     sizeof(VkDrawIndexedIndirectCommand));
        }

        void destroy() noexcept {
            pipelineFactory.destroy(shadowPipeline);
            pipelineFactory.destroy(gbufferPipeline);
            if (descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(context.device(), descriptorPool, nullptr);
                descriptorPool = VK_NULL_HANDLE;
            }
            if (descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(context.device(), descriptorSetLayout, nullptr);
                descriptorSetLayout = VK_NULL_HANDLE;
            }
            for (VulkanBuffer& buffer : shadowUniformBuffers) {
                resources.destroyBuffer(buffer);
            }
            for (VulkanBuffer& buffer : uniformBuffers) {
                resources.destroyBuffer(buffer);
            }
            for (VulkanBuffer& buffer : objectBuffers) {
                resources.destroyBuffer(buffer);
            }
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
            const glm::mat4 modelMatrix = model.transform.matrix();
            batch.objects.push_back(ObjectData{
                .model = modelMatrix,
                .previousModel = modelMatrix,
                .normalMatrix = glm::inverseTranspose(modelMatrix),
                .albedoRoughness = glm::vec4{model.material.albedo, model.material.roughness},
            });
        }

        if (batch.commands.size() != batch.objects.size()) {
            throw std::logic_error("Indirect command and object data counts diverged.");
        }
        return batch;
    }

    ModelRenderer::ModelRenderer(VulkanContext& context, const scene::Level& level,
                                 std::filesystem::path shaderDirectory, std::span<const VkFormat> colorFormats,
                                 VkFormat depthFormat, VkFormat shadowDepthFormat, std::uint32_t frameCount)
        : impl_(std::make_unique<Impl>(context, level, std::move(shaderDirectory), colorFormats, depthFormat,
                                       shadowDepthFormat, frameCount)) {
    }

    ModelRenderer::~ModelRenderer() = default;

    void ModelRenderer::sync(const scene::Level& level, std::uint32_t frameIndex, bool resetMotion) {
        if (frameIndex >= impl_->frameCount) {
            throw std::out_of_range("ModelRenderer sync frame index is out of range.");
        }
        if (level.models().size() != impl_->batch.objects.size()) {
            throw std::logic_error("Level topology changed without rebuilding ModelRenderer.");
        }

        for (std::size_t index = 0; index < level.models().size(); ++index) {
            const scene::ModelInstance& model = level.models()[index];
            const glm::mat4 currentModel = model.transform.matrix();
            const glm::mat4 previousModel =
                resetMotion || !impl_->hasPreviousModels ? currentModel : impl_->previousModels[index];
            impl_->batch.objects[index] = ObjectData{
                .model = currentModel,
                .previousModel = previousModel,
                .normalMatrix = glm::inverseTranspose(currentModel),
                .albedoRoughness = glm::vec4{model.material.albedo, model.material.roughness},
            };
            impl_->previousModels[index] = currentModel;
        }
        impl_->hasPreviousModels = true;
        impl_->resources.writeBuffer(impl_->objectBuffers[frameIndex], impl_->batch.objects.data(),
                                     sizeof(ObjectData) * impl_->batch.objects.size());
    }

    void ModelRenderer::recordGBuffer(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                      const glm::mat4& viewProjection, const glm::mat4& previousViewProjection) {
        if (frameIndex >= impl_->frameCount) {
            throw std::out_of_range("ModelRenderer frame index is out of range.");
        }
        const Impl::FrameUniforms uniforms{viewProjection, previousViewProjection};
        impl_->resources.writeBuffer(impl_->uniformBuffers[frameIndex], &uniforms, sizeof(uniforms));
        impl_->bindGeometryAndDraw(commandBuffer, impl_->gbufferPipeline, impl_->descriptorSets[frameIndex]);
    }

    void ModelRenderer::recordShadow(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                     std::uint32_t cascadeIndex, const glm::mat4& lightViewProjection) {
        if (frameIndex >= impl_->frameCount || cascadeIndex >= cascadeCount) {
            throw std::out_of_range("ModelRenderer shadow frame or cascade index is out of range.");
        }
        const std::uint32_t index = impl_->shadowIndex(frameIndex, cascadeIndex);
        const Impl::ShadowUniforms uniforms{lightViewProjection};
        impl_->resources.writeBuffer(impl_->shadowUniformBuffers[index], &uniforms, sizeof(uniforms));
        impl_->bindGeometryAndDraw(commandBuffer, impl_->shadowPipeline, impl_->shadowDescriptorSets[index]);
    }

    void ModelRenderer::record(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, const scene::Camera& camera,
                               float aspectRatio) {
        const glm::mat4 viewProjection = camera.projectionMatrix(aspectRatio) * camera.viewMatrix();
        recordGBuffer(commandBuffer, frameIndex, viewProjection, viewProjection);
    }

    std::uint32_t ModelRenderer::drawCount() const noexcept {
        return static_cast<std::uint32_t>(impl_->batch.commands.size());
    }

} // namespace lumin::render

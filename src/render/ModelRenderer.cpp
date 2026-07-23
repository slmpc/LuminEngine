#include "lumin/render/ModelRenderer.hpp"

#include "DescriptorIndexingLimits.hpp"
#include "lumin/assets/ImageLoader.hpp"
#include "lumin/render/PipelineFactory.hpp"
#include "lumin/render/ShaderLibrary.hpp"
#include "lumin/render/VulkanContext.hpp"
#include "lumin/render/VulkanResources.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/Level.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
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

        std::vector<scene::PbrTextureSet> collectTextureSets(const scene::Level& level) {
            std::vector<scene::PbrTextureSet> textureSets;
            for (const scene::ModelInstance& model : level.models()) {
                if (!model.material.textures.has_value()) {
                    continue;
                }
                const auto existing = std::find_if(textureSets.begin(), textureSets.end(), [&](const auto& candidate) {
                    return candidate.referencesSameImages(*model.material.textures);
                });
                if (existing == textureSets.end()) {
                    textureSets.push_back(*model.material.textures);
                }
            }
            return textureSets;
        }

        std::uint32_t textureDescriptorIndexFor(const scene::Material& material,
                                                const std::vector<scene::PbrTextureSet>& textureSets) {
            if (!material.textures.has_value()) {
                return 0;
            }
            const auto iterator = std::find_if(textureSets.begin(), textureSets.end(), [&](const auto& candidate) {
                return candidate.referencesSameImages(*material.textures);
            });
            if (iterator == textureSets.end()) {
                throw std::logic_error("A model references a PBR texture set that was not loaded.");
            }
            const std::size_t descriptorIndex =
                static_cast<std::size_t>(std::distance(textureSets.begin(), iterator)) + 1;
            if (descriptorIndex >= detail::maxMaterialTextureDescriptorCount) {
                throw std::length_error("Material texture descriptor index exceeds exact float representation.");
            }
            return checkedU32(descriptorIndex, "Material texture descriptor index exceeds Vulkan's uint32 range.");
        }

        ObjectData makeObjectData(const scene::ModelInstance& model, const glm::mat4& currentModel,
                                  const glm::mat4& previousModel,
                                  const std::vector<scene::PbrTextureSet>& textureSets) {
            const float normalYSign =
                model.material.textures.has_value() && model.material.textures->flipNormalY ? -1.0f : 1.0f;
            return ObjectData{
                .model = currentModel,
                .previousModel = previousModel,
                .normalMatrix = glm::inverseTranspose(currentModel),
                .baseColorMetallic = glm::vec4{model.material.albedo, model.material.metallic},
                .materialParameters =
                    glm::vec4{model.material.roughness, model.material.textureScale,
                              static_cast<float>(textureDescriptorIndexFor(model.material, textureSets)), normalYSign},
            };
        }

        struct LoadedPbrTextureSet {
            assets::ImageData baseColor;
            assets::ImageData normal;
            assets::ImageData roughness;
        };

        LoadedPbrTextureSet loadTextureSet(const scene::PbrTextureSet& paths) {
            if (paths.baseColor.empty() || paths.normal.empty() || paths.roughness.empty()) {
                throw std::invalid_argument("PBR texture sets require base-color, normal, and roughness images.");
            }

            LoadedPbrTextureSet result{
                assets::ImageLoader::load(paths.baseColor),
                assets::ImageLoader::load(paths.normal),
                assets::ImageLoader::load(paths.roughness),
            };
            if (result.normal.width != result.roughness.width || result.normal.height != result.roughness.height) {
                throw std::invalid_argument("Normal and roughness images in a PBR texture set must match dimensions.");
            }
            return result;
        }

        std::vector<std::uint8_t> packNormalRoughness(const LoadedPbrTextureSet& textureSet) {
            if (textureSet.normal.pixels.size() != textureSet.roughness.pixels.size() ||
                textureSet.normal.pixels.size() % 4 != 0) {
                throw std::invalid_argument("Normal and roughness images must contain matching RGBA8 pixels.");
            }

            std::vector<std::uint8_t> pixels = textureSet.normal.pixels;
            for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
                pixels[offset + 3] = textureSet.roughness.pixels[offset];
            }
            return pixels;
        }

        std::array<VkVertexInputAttributeDescription, 3> vertexAttributes() {
            return {
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                  static_cast<std::uint32_t>(offsetof(assets::Vertex, position))},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                  static_cast<std::uint32_t>(offsetof(assets::Vertex, normal))},
                VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT,
                                                  static_cast<std::uint32_t>(offsetof(assets::Vertex, texCoord))},
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
        std::vector<scene::PbrTextureSet> textureSets;
        ModelBatch batch;
        std::uint32_t frameCount = 0;
        VkPhysicalDeviceProperties deviceProperties{};
        detail::DescriptorIndexingPlan descriptorPlan;

        VkDescriptorSetLayout gbufferDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout shadowDescriptorSetLayout = VK_NULL_HANDLE;
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
        std::vector<VulkanImage> baseColorTextures;
        std::vector<VulkanImage> normalRoughnessTextures;
        VkSampler materialSampler = VK_NULL_HANDLE;
        std::vector<glm::mat4> previousModels;
        bool hasPreviousModels = false;

        Impl(VulkanContext& contextValue, const scene::Level& level, std::filesystem::path shaderDirectory,
             std::span<const VkFormat> colorFormats, VkFormat depthFormat, VkFormat shadowDepthFormat,
             std::uint32_t frameCountValue)
            : context(contextValue), resources(contextValue),
              shaders(contextValue.device(), std::move(shaderDirectory)), pipelineFactory(contextValue.device()),
              textureSets(collectTextureSets(level)), batch(ModelRenderer::buildBatch(level)),
              frameCount(frameCountValue) {
            try {
                if (batch.commands.empty()) {
                    throw std::invalid_argument("ModelRenderer requires at least one model.");
                }
                if (frameCount == 0) {
                    throw std::invalid_argument("ModelRenderer requires at least one frame slot.");
                }

                vkGetPhysicalDeviceProperties(context.physicalDevice(), &deviceProperties);
                if (batch.commands.size() > deviceProperties.limits.maxDrawIndirectCount) {
                    throw std::length_error("Level model count exceeds maxDrawIndirectCount.");
                }
                descriptorPlan = detail::makeDescriptorIndexingPlan(deviceProperties.limits, textureSets.size(),
                                                                    frameCount, cascadeCount);

                previousModels.reserve(batch.objects.size());
                for (const ObjectData& object : batch.objects) {
                    previousModels.push_back(object.model);
                }
                createBuffers();
                createMaterialTextures();
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

        VulkanImage createUploadedMaterialImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                                                std::span<const std::uint8_t> pixels) {
            detail::validateMaterialImageDimensions(deviceProperties.limits, width, height);
            constexpr std::uint64_t bytesPerPixel = 4;
            const std::uint64_t pixelCount = static_cast<std::uint64_t>(width) * height;
            if (pixelCount > std::numeric_limits<std::uint64_t>::max() / bytesPerPixel) {
                throw std::overflow_error("Material texture byte count exceeds uint64 range.");
            }
            const std::uint64_t byteCount = pixelCount * bytesPerPixel;
            if (byteCount > std::numeric_limits<std::size_t>::max()) {
                throw std::overflow_error("Material texture byte count exceeds host size range.");
            }
            if (pixels.size() != static_cast<std::size_t>(byteCount)) {
                throw std::invalid_argument("Material texture pixels do not match the declared dimensions.");
            }

            const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            VulkanImage image = resources.createImage(width, height, format, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                                                      VK_IMAGE_VIEW_TYPE_2D);
            try {
                resources.uploadImage(image, pixels.data(), static_cast<VkDeviceSize>(byteCount));
            } catch (...) {
                resources.destroyImage(image);
                throw;
            }
            return image;
        }

        void createMaterialTextures() {
            baseColorTextures.reserve(descriptorPlan.materialTextureCount);
            normalRoughnessTextures.reserve(descriptorPlan.materialTextureCount);

            constexpr std::array<std::uint8_t, 4> fallbackBaseColor = {255, 255, 255, 255};
            constexpr std::array<std::uint8_t, 4> fallbackNormalRoughness = {128, 128, 255, 255};
            baseColorTextures.push_back(createUploadedMaterialImage(1, 1, VK_FORMAT_R8G8B8A8_SRGB, fallbackBaseColor));
            normalRoughnessTextures.push_back(
                createUploadedMaterialImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, fallbackNormalRoughness));

            for (const scene::PbrTextureSet& textureSet : textureSets) {
                const LoadedPbrTextureSet loaded = loadTextureSet(textureSet);
                const std::vector<std::uint8_t> normalRoughnessPixels = packNormalRoughness(loaded);
                baseColorTextures.push_back(createUploadedMaterialImage(
                    loaded.baseColor.width, loaded.baseColor.height, VK_FORMAT_R8G8B8A8_SRGB, loaded.baseColor.pixels));
                normalRoughnessTextures.push_back(createUploadedMaterialImage(
                    loaded.normal.width, loaded.normal.height, VK_FORMAT_R8G8B8A8_UNORM, normalRoughnessPixels));
            }

            if (baseColorTextures.size() != descriptorPlan.materialTextureCount ||
                normalRoughnessTextures.size() != descriptorPlan.materialTextureCount) {
                throw std::logic_error("Material image and descriptor counts diverged.");
            }

            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = 0.0f;
            checkVk(vkCreateSampler(context.device(), &samplerInfo, nullptr, &materialSampler),
                    "Failed to create the material texture sampler.");
        }

        void createDescriptors() {
            const std::array<VkDescriptorSetLayoutBinding, 5> gbufferBindings = {
                VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                             nullptr},
                VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                             nullptr},
                VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, descriptorPlan.materialTextureCount,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, descriptorPlan.materialTextureCount,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            };
            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<std::uint32_t>(gbufferBindings.size());
            layoutInfo.pBindings = gbufferBindings.data();
            checkVk(vkCreateDescriptorSetLayout(context.device(), &layoutInfo, nullptr, &gbufferDescriptorSetLayout),
                    "Failed to create G-buffer model descriptor set layout.");

            const std::array<VkDescriptorSetLayoutBinding, 2> shadowBindings = {
                VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                             nullptr},
                VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                             nullptr},
            };
            layoutInfo.bindingCount = static_cast<std::uint32_t>(shadowBindings.size());
            layoutInfo.pBindings = shadowBindings.data();
            checkVk(vkCreateDescriptorSetLayout(context.device(), &layoutInfo, nullptr, &shadowDescriptorSetLayout),
                    "Failed to create shadow model descriptor set layout.");

            const std::array<VkDescriptorPoolSize, 4> poolSizes = {
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descriptorPlan.totalSetCount},
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptorPlan.totalSetCount},
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, descriptorPlan.sampledImageDescriptorCount},
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, descriptorPlan.samplerDescriptorCount},
            };
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = descriptorPlan.totalSetCount;
            poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            checkVk(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr, &descriptorPool),
                    "Failed to create model descriptor pool.");

            std::vector<VkDescriptorSetLayout> layouts;
            layouts.reserve(descriptorPlan.totalSetCount);
            layouts.insert(layouts.end(), descriptorPlan.gbufferSetCount, gbufferDescriptorSetLayout);
            layouts.insert(layouts.end(), descriptorPlan.shadowSetCount, shadowDescriptorSetLayout);
            std::vector<VkDescriptorSet> allocatedSets(descriptorPlan.totalSetCount);
            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.descriptorPool = descriptorPool;
            allocateInfo.descriptorSetCount = descriptorPlan.totalSetCount;
            allocateInfo.pSetLayouts = layouts.data();
            checkVk(vkAllocateDescriptorSets(context.device(), &allocateInfo, allocatedSets.data()),
                    "Failed to allocate model descriptor sets.");

            const auto shadowBegin = allocatedSets.begin() + descriptorPlan.gbufferSetCount;
            descriptorSets.assign(allocatedSets.begin(), shadowBegin);
            shadowDescriptorSets.assign(shadowBegin, allocatedSets.end());

            std::vector<VkDescriptorImageInfo> baseColorInfos;
            std::vector<VkDescriptorImageInfo> normalRoughnessInfos;
            baseColorInfos.reserve(descriptorPlan.materialTextureCount);
            normalRoughnessInfos.reserve(descriptorPlan.materialTextureCount);
            for (std::uint32_t textureIndex = 0; textureIndex < descriptorPlan.materialTextureCount; ++textureIndex) {
                baseColorInfos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE, baseColorTextures[textureIndex].view,
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                normalRoughnessInfos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE,
                                                                     normalRoughnessTextures[textureIndex].view,
                                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }

            auto updateGBufferSet = [&](VkDescriptorSet set, const VulkanBuffer& uniform, const VulkanBuffer& objects) {
                const VkDescriptorBufferInfo uniformInfo{uniform.buffer, 0, sizeof(FrameUniforms)};
                const VkDescriptorBufferInfo objectInfo{objects.buffer, 0, objects.size};
                const VkDescriptorImageInfo samplerInfo{materialSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                const std::array<VkWriteDescriptorSet, 5> writes = {
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniformInfo, nullptr},
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
                                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objectInfo, nullptr},
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0,
                                         descriptorPlan.materialTextureCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                         baseColorInfos.data(), nullptr, nullptr},
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0,
                                         descriptorPlan.materialTextureCount, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                         normalRoughnessInfos.data(), nullptr, nullptr},
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 4, 0, 1,
                                         VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr, nullptr},
                };
                vkUpdateDescriptorSets(context.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                                       nullptr);
            };

            auto updateShadowSet = [&](VkDescriptorSet set, const VulkanBuffer& uniform, const VulkanBuffer& objects) {
                const VkDescriptorBufferInfo uniformInfo{uniform.buffer, 0, sizeof(ShadowUniforms)};
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
                updateGBufferSet(descriptorSets[frameIndex], uniformBuffers[frameIndex], objectBuffers[frameIndex]);
                for (std::uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
                    const std::uint32_t index = shadowIndex(frameIndex, cascadeIndex);
                    updateShadowSet(shadowDescriptorSets[index], shadowUniformBuffers[index],
                                    objectBuffers[frameIndex]);
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
                desc.descriptorSetLayout = gbufferDescriptorSetLayout;
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
                desc.descriptorSetLayout = shadowDescriptorSetLayout;
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
            if (shadowDescriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(context.device(), shadowDescriptorSetLayout, nullptr);
                shadowDescriptorSetLayout = VK_NULL_HANDLE;
            }
            if (gbufferDescriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(context.device(), gbufferDescriptorSetLayout, nullptr);
                gbufferDescriptorSetLayout = VK_NULL_HANDLE;
            }
            if (materialSampler != VK_NULL_HANDLE) {
                vkDestroySampler(context.device(), materialSampler, nullptr);
                materialSampler = VK_NULL_HANDLE;
            }
            for (VulkanImage& texture : normalRoughnessTextures) {
                resources.destroyImage(texture);
            }
            for (VulkanImage& texture : baseColorTextures) {
                resources.destroyImage(texture);
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
        const std::vector<scene::PbrTextureSet> textureSets = collectTextureSets(level);
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
            batch.objects.push_back(makeObjectData(model, modelMatrix, modelMatrix, textureSets));
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
            impl_->batch.objects[index] = makeObjectData(model, currentModel, previousModel, impl_->textureSets);
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

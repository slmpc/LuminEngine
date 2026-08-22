#include "render/ModelRenderer.hpp"

#include "assets/ImageLoader.hpp"
#include "render/gpu/GpuMaterial.hpp"
#include "render/gpu/GpuScene.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"
#include "render/resources/DescriptorIndexingLimits.hpp"
#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"
#include "render/resources/VulkanResources.hpp"
#include "render/world/RenderWorld.hpp"
#include "scene/Camera.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
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

        std::vector<scene::PbrTextureSet> collectTextureSets(const world::RenderWorldSnapshot& worldSnapshot) {
            std::vector<scene::PbrTextureSet> textureSets;
            for (const world::RenderWorldInstance& instance : worldSnapshot.instances()) {
                const scene::ModelInstance& model = instance.model;
                if (!model.material.textures.has_value() || !model.material.textures->complete()) {
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
            if (!material.textures.has_value() || !material.textures->complete()) {
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
            return checkedU32(descriptorIndex, "Material texture descriptor index exceeds the uint32 range.");
        }

        ObjectData makeObjectData(const scene::ModelInstance& model, const glm::mat4& currentModel,
                                  const glm::mat4& previousModel, const std::vector<scene::PbrTextureSet>& textureSets,
                                  gpu::GpuMaterialIndex materialIndex) {
            if (!materialIndex.isValid()) {
                throw std::invalid_argument("Model object data requires a valid GPU material index.");
            }
            const std::uint32_t textureDescriptorIndex = textureDescriptorIndexFor(model.material, textureSets);
            const gpu::GpuMaterialData material = gpu::packGpuMaterial(model.material, textureDescriptorIndex);
            return ObjectData{
                .model = currentModel,
                .previousModel = previousModel,
                .normalMatrix = glm::inverseTranspose(currentModel),
                .baseColorMetallic = material.baseColorMetallic,
                // 旧 raster object ABI 暂时读取统一等效粗糙度；完整表面模型参数由 GpuMaterialData 提供。
                .materialParameters = glm::vec4{material.surfaceParameters.x, material.surfaceParameters.y,
                                                static_cast<float>(material.metadata.y), material.surfaceParameters.z},
                .metadata = glm::uvec4{materialIndex.value(), 0U, 0U, 0U},
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

        std::array<nvrhi::VertexAttributeDesc, 3> vertexAttributes() {
            return {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<std::uint32_t>(offsetof(assets::Vertex, position)))
                    .setElementStride(sizeof(assets::Vertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("NORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<std::uint32_t>(offsetof(assets::Vertex, normal)))
                    .setElementStride(sizeof(assets::Vertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<std::uint32_t>(offsetof(assets::Vertex, texCoord)))
                    .setElementStride(sizeof(assets::Vertex)),
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

        nvrhi::IDevice& device;
        GpuResourceManager resources;
        ShaderLibrary shaders;
        PipelineFactory pipelineFactory;
        std::vector<scene::PbrTextureSet> textureSets;
        std::vector<LoadedPbrTextureSet> loadedTextureSets;
        ModelBatch batch;
        std::uint32_t frameCount = 0;
        detail::DescriptorIndexingLimits limits;
        detail::DescriptorIndexingPlan descriptorPlan;
        detail::ModelRendererBindingContract bindingContract;

        nvrhi::BindingLayoutHandle gbufferBindingLayout;
        nvrhi::BindingLayoutHandle shadowBindingLayout;
        std::vector<nvrhi::BindingSetHandle> gbufferBindingSets;
        std::vector<nvrhi::BindingSetHandle> shadowBindingSets;
        nvrhi::GraphicsPipelineHandle gbufferPipeline;
        nvrhi::GraphicsPipelineHandle shadowPipeline;
        nvrhi::InputLayoutHandle gbufferInputLayout;
        nvrhi::InputLayoutHandle shadowInputLayout;
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        nvrhi::BufferHandle indirectBuffer;
        std::vector<nvrhi::BufferHandle> objectBuffers;
        std::vector<nvrhi::BufferHandle> materialBuffers;
        std::vector<nvrhi::BufferHandle> uniformBuffers;
        std::vector<nvrhi::BufferHandle> shadowUniformBuffers;
        std::vector<nvrhi::TextureHandle> baseColorTextures;
        std::vector<nvrhi::TextureHandle> normalRoughnessTextures;
        nvrhi::SamplerHandle materialSampler;
        std::vector<glm::mat4> previousModels;
        std::vector<glm::mat4> pendingModels;
        detail::FrameSlotReadiness frameSlotReadiness;
        std::vector<bool> materialBufferInitialized;
        std::uint32_t pendingFrameIndex = std::numeric_limits<std::uint32_t>::max();
        bool hasPreviousModels = false;
        bool hasPendingModels = false;

        Impl(VulkanContext& context, const world::RenderWorldSnapshot& worldSnapshot,
             std::filesystem::path shaderDirectory, std::span<const nvrhi::Format> colorFormats,
             nvrhi::Format depthFormat, nvrhi::Format shadowDepthFormat, std::uint32_t frameCountValue,
             ModelRendererCapabilities capabilities)
            : device(*context.rhiDevice()), resources(device), shaders(device, std::move(shaderDirectory)),
              pipelineFactory(device), textureSets(collectTextureSets(worldSnapshot)),
              batch(ModelRenderer::buildBatch(worldSnapshot)), frameCount(frameCountValue),
              limits(detail::toDescriptorIndexingLimits(capabilities)), frameSlotReadiness(frameCountValue),
              materialBufferInitialized(frameCountValue, false) {
            if (batch.commands.empty()) {
                throw std::invalid_argument("ModelRenderer requires at least one model.");
            }
            const std::vector<detail::ModelRendererMaterialImageDimensions> materialImages = preloadMaterialImages();
            descriptorPlan = detail::createModelRendererResourcesAfterPreflight(
                capabilities, textureSets.size(), frameCount, cascadeCount, batch.commands.size(), materialImages,
                [&](const detail::DescriptorIndexingPlan& plan) {
                    descriptorPlan = plan;
                    bindingContract = detail::makeModelRendererBindingContract(plan);
                    previousModels.reserve(batch.objects.size());
                    pendingModels.reserve(batch.objects.size());
                    for (const ObjectData& object : batch.objects) {
                        previousModels.push_back(object.model);
                        pendingModels.push_back(object.model);
                    }
                    createBuffers();
                    createMaterialTextures();
                    createBindingLayouts();
                    createBindingSets();
                    createPipelines(colorFormats, depthFormat, shadowDepthFormat);
                });
        }

        [[nodiscard]] std::uint32_t shadowIndex(std::uint32_t frameIndex, std::uint32_t cascadeIndex) const {
            return frameIndex * cascadeCount + cascadeIndex;
        }

        [[nodiscard]] nvrhi::BufferHandle createStaticBuffer(nvrhi::BufferDesc desc, const void* data,
                                                             std::size_t size) {
            GpuBuffer buffer = resources.createStaticBuffer(desc, data, size);
            return std::move(buffer.buffer);
        }

        [[nodiscard]] nvrhi::BufferHandle createCpuBuffer(nvrhi::BufferDesc desc) {
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            GpuBuffer buffer = resources.createBuffer(desc);
            return std::move(buffer.buffer);
        }

        [[nodiscard]] std::vector<detail::ModelRendererMaterialImageDimensions> preloadMaterialImages() {
            std::vector<detail::ModelRendererMaterialImageDimensions> dimensions;
            dimensions.reserve(textureSets.size() * 3);
            loadedTextureSets.reserve(textureSets.size());
            for (const scene::PbrTextureSet& textureSet : textureSets) {
                LoadedPbrTextureSet loaded = loadTextureSet(textureSet);
                dimensions.push_back({loaded.baseColor.width, loaded.baseColor.height});
                dimensions.push_back({loaded.normal.width, loaded.normal.height});
                dimensions.push_back({loaded.roughness.width, loaded.roughness.height});
                loadedTextureSets.push_back(std::move(loaded));
            }
            return dimensions;
        }

        void writeCpuBuffer(const nvrhi::BufferHandle& buffer, const void* data, std::size_t size) {
            if (!buffer || data == nullptr || size == 0 || size > buffer->getDesc().byteSize) {
                throw std::invalid_argument("Model buffer write requires non-empty in-bounds data.");
            }
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map a ModelRenderer buffer.");
            }
            std::memcpy(mapped, data, size);
            device.unmapBuffer(buffer);
        }

        void createBuffers() {
            const std::size_t vertexBytes = sizeof(assets::Vertex) * batch.vertices.size();
            const std::size_t indexBytes = sizeof(std::uint32_t) * batch.indices.size();
            const std::size_t indirectBytes = sizeof(ModelBatch::Command) * batch.commands.size();
            const std::size_t objectBytes = sizeof(ObjectData) * batch.objects.size();
            const std::size_t materialBytes = sizeof(gpu::GpuMaterialData) * batch.materials.size();

            nvrhi::BufferDesc desc;
            desc.byteSize = vertexBytes;
            desc.debugName = "Model vertices";
            desc.isVertexBuffer = true;
            desc.initialState = nvrhi::ResourceStates::VertexBuffer;
            desc.keepInitialState = true;
            vertexBuffer = createStaticBuffer(desc, batch.vertices.data(), vertexBytes);

            desc = {};
            desc.byteSize = indexBytes;
            desc.debugName = "Model indices";
            desc.isIndexBuffer = true;
            desc.initialState = nvrhi::ResourceStates::IndexBuffer;
            desc.keepInitialState = true;
            indexBuffer = createStaticBuffer(desc, batch.indices.data(), indexBytes);

            desc = {};
            desc.byteSize = indirectBytes;
            desc.debugName = "Model indirect commands";
            desc.isDrawIndirectArgs = true;
            desc.initialState = nvrhi::ResourceStates::IndirectArgument;
            desc.keepInitialState = true;
            indirectBuffer = createStaticBuffer(desc, batch.commands.data(), indirectBytes);

            objectBuffers.reserve(frameCount);
            materialBuffers.reserve(frameCount);
            uniformBuffers.reserve(frameCount);
            for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                desc = {};
                desc.byteSize = objectBytes;
                desc.structStride = sizeof(ObjectData);
                desc.debugName = "Model objects " + std::to_string(frameIndex);
                nvrhi::BufferHandle objects = createCpuBuffer(desc);
                writeCpuBuffer(objects, batch.objects.data(), objectBytes);
                objectBuffers.push_back(std::move(objects));

                desc = {};
                desc.byteSize = materialBytes;
                desc.structStride = sizeof(gpu::GpuMaterialData);
                desc.debugName = "Model materials " + std::to_string(frameIndex);
                nvrhi::BufferHandle materials = createCpuBuffer(desc);
                writeCpuBuffer(materials, batch.materials.data(), materialBytes);
                materialBuffers.push_back(std::move(materials));

                desc = {};
                desc.byteSize = sizeof(FrameUniforms);
                desc.debugName = "Model frame uniforms " + std::to_string(frameIndex);
                desc.isConstantBuffer = true;
                uniformBuffers.push_back(createCpuBuffer(desc));
            }

            shadowUniformBuffers.reserve(frameCount * cascadeCount);
            for (std::uint32_t index = 0; index < frameCount * cascadeCount; ++index) {
                desc = {};
                desc.byteSize = sizeof(ShadowUniforms);
                desc.debugName = "Model shadow uniforms " + std::to_string(index);
                desc.isConstantBuffer = true;
                shadowUniformBuffers.push_back(createCpuBuffer(desc));
            }
        }

        [[nodiscard]] nvrhi::TextureHandle createUploadedMaterialImage(std::uint32_t width, std::uint32_t height,
                                                                       nvrhi::Format format,
                                                                       std::span<const std::uint8_t> pixels) {
            detail::validateMaterialImageDimensions(limits, width, height);
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

            nvrhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.dimension = nvrhi::TextureDimension::Texture2D;
            desc.isShaderResource = true;
            desc.debugName = "Model material texture";
            GpuTexture texture = resources.createTexture(desc);
            resources.uploadTexture(texture, pixels.data(), static_cast<std::size_t>(width) * bytesPerPixel,
                                    static_cast<std::size_t>(byteCount));
            return std::move(texture.texture);
        }

        void createMaterialTextures() {
            baseColorTextures.reserve(descriptorPlan.materialTextureCount);
            normalRoughnessTextures.reserve(descriptorPlan.materialTextureCount);

            constexpr std::array<std::uint8_t, 4> fallbackBaseColor = {255, 255, 255, 255};
            constexpr std::array<std::uint8_t, 4> fallbackNormalRoughness = {128, 128, 255, 255};
            baseColorTextures.push_back(
                createUploadedMaterialImage(1, 1, nvrhi::Format::SRGBA8_UNORM, fallbackBaseColor));
            normalRoughnessTextures.push_back(
                createUploadedMaterialImage(1, 1, nvrhi::Format::RGBA8_UNORM, fallbackNormalRoughness));

            for (const LoadedPbrTextureSet& loaded : loadedTextureSets) {
                const std::vector<std::uint8_t> normalRoughnessPixels = packNormalRoughness(loaded);
                baseColorTextures.push_back(createUploadedMaterialImage(loaded.baseColor.width, loaded.baseColor.height,
                                                                        nvrhi::Format::SRGBA8_UNORM,
                                                                        loaded.baseColor.pixels));
                normalRoughnessTextures.push_back(createUploadedMaterialImage(
                    loaded.normal.width, loaded.normal.height, nvrhi::Format::RGBA8_UNORM, normalRoughnessPixels));
            }

            if (baseColorTextures.size() != descriptorPlan.materialTextureCount ||
                normalRoughnessTextures.size() != descriptorPlan.materialTextureCount) {
                throw std::logic_error("Material image and binding counts diverged.");
            }

            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Repeat);
            materialSampler = device.createSampler(samplerDesc);
            if (!materialSampler) {
                throw std::runtime_error("Failed to create the material sampler.");
            }
        }

        void createBindingLayouts() {
            nvrhi::VulkanBindingOffsets offsets;
            offsets.setShaderResourceOffset(0)
                .setSamplerOffset(0)
                .setConstantBufferOffset(0)
                .setUnorderedAccessViewOffset(0);

            nvrhi::BindingLayoutDesc gbufferDesc;
            gbufferDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(offsets)
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(bindingContract.gbufferItems[0].binding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(bindingContract.gbufferItems[1].binding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(bindingContract.gbufferItems[2].binding)
                             .setSize(bindingContract.gbufferItems[2].arrayLength))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(bindingContract.gbufferItems[3].binding)
                             .setSize(bindingContract.gbufferItems[3].arrayLength))
                .addItem(nvrhi::BindingLayoutItem::Sampler(bindingContract.gbufferItems[4].binding));
            gbufferBindingLayout = device.createBindingLayout(gbufferDesc);

            nvrhi::BindingLayoutDesc shadowDesc;
            shadowDesc.setVisibility(nvrhi::ShaderType::Vertex)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(offsets)
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(bindingContract.shadowItems[0].binding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(bindingContract.shadowItems[1].binding));
            shadowBindingLayout = device.createBindingLayout(shadowDesc);
            if (!gbufferBindingLayout || !shadowBindingLayout) {
                throw std::runtime_error("Failed to create model binding layouts.");
            }
        }

        [[nodiscard]] nvrhi::BindingSetHandle createGBufferBindingSet(std::uint32_t frameIndex) {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(bindingContract.gbufferItems[0].binding,
                                                               uniformBuffers[frameIndex]))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(bindingContract.gbufferItems[1].binding,
                                                                     objectBuffers[frameIndex]));
            detail::forEachMaterialTextureArrayElement(
                bindingContract,
                [&](std::uint32_t baseColorBinding, std::uint32_t normalRoughnessBinding, std::uint32_t textureIndex) {
                    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(baseColorBinding, baseColorTextures[textureIndex])
                                     .setArrayElement(textureIndex));
                    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(normalRoughnessBinding,
                                                                    normalRoughnessTextures[textureIndex])
                                     .setArrayElement(textureIndex));
                });
            desc.addItem(nvrhi::BindingSetItem::Sampler(bindingContract.gbufferItems[4].binding, materialSampler));
            return device.createBindingSet(desc, gbufferBindingLayout);
        }

        void createBindingSets() {
            gbufferBindingSets.reserve(frameCount);
            shadowBindingSets.reserve(frameCount * cascadeCount);
            for (std::uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                nvrhi::BindingSetHandle gbufferSet = createGBufferBindingSet(frameIndex);
                if (!gbufferSet) {
                    throw std::runtime_error("Failed to create a G-buffer model binding set.");
                }
                gbufferBindingSets.push_back(std::move(gbufferSet));
                for (std::uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
                    const std::uint32_t index = shadowIndex(frameIndex, cascadeIndex);
                    nvrhi::BindingSetDesc desc;
                    desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(bindingContract.shadowItems[0].binding,
                                                                       shadowUniformBuffers[index]))
                        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(bindingContract.shadowItems[1].binding,
                                                                             objectBuffers[frameIndex]));
                    nvrhi::BindingSetHandle shadowSet = device.createBindingSet(desc, shadowBindingLayout);
                    if (!shadowSet) {
                        throw std::runtime_error("Failed to create a shadow model binding set.");
                    }
                    shadowBindingSets.push_back(std::move(shadowSet));
                }
            }
        }

        void createPipelines(std::span<const nvrhi::Format> colorFormats, nvrhi::Format depthFormat,
                             nvrhi::Format shadowDepthFormat) {
            const nvrhi::ShaderHandle gbufferVertex =
                shaders.loadModule("GBuffer.vert.spv", nvrhi::ShaderType::Vertex, "vertexMain");
            const nvrhi::ShaderHandle gbufferFragment =
                shaders.loadModule("GBuffer.frag.spv", nvrhi::ShaderType::Pixel, "fragmentMain");
            const auto attributes = vertexAttributes();
            gbufferInputLayout = device.createInputLayout(attributes.data(),
                                                          static_cast<std::uint32_t>(attributes.size()), gbufferVertex);
            if (!gbufferInputLayout) {
                throw std::runtime_error("Failed to create the G-buffer model input layout.");
            }
            const std::array<nvrhi::BindingLayoutHandle, 1> gbufferLayouts = {gbufferBindingLayout};
            GraphicsPipelineDesc gbufferDesc;
            gbufferDesc.vertexShader = gbufferVertex;
            gbufferDesc.fragmentShader = gbufferFragment;
            gbufferDesc.inputLayout = gbufferInputLayout;
            gbufferDesc.bindingLayouts = gbufferLayouts;
            gbufferDesc.colorFormats = colorFormats;
            gbufferDesc.depthFormat = depthFormat;
            gbufferDesc.cullMode = nvrhi::RasterCullMode::None;
            gbufferPipeline = pipelineFactory.createGraphicsPipeline(gbufferDesc);

            const nvrhi::ShaderHandle shadowVertex =
                shaders.loadModule("Shadow.vert.spv", nvrhi::ShaderType::Vertex, "vertexMain");
            const std::array<nvrhi::VertexAttributeDesc, 1> shadowAttributes = {attributes[0]};
            shadowInputLayout = device.createInputLayout(
                shadowAttributes.data(), static_cast<std::uint32_t>(shadowAttributes.size()), shadowVertex);
            if (!shadowInputLayout) {
                throw std::runtime_error("Failed to create the shadow model input layout.");
            }
            const std::array<nvrhi::BindingLayoutHandle, 1> shadowLayouts = {shadowBindingLayout};
            const std::array<nvrhi::Format, 0> noColors{};
            GraphicsPipelineDesc shadowDesc;
            shadowDesc.vertexShader = shadowVertex;
            shadowDesc.inputLayout = shadowInputLayout;
            shadowDesc.bindingLayouts = shadowLayouts;
            shadowDesc.colorFormats = noColors;
            shadowDesc.depthFormat = shadowDepthFormat;
            shadowDesc.depthCompareOp = nvrhi::ComparisonFunc::LessOrEqual;
            shadowDesc.cullMode = nvrhi::RasterCullMode::None;
            shadowDesc.depthBiasEnable = true;
            shadowDesc.depthBias = 1;
            shadowDesc.slopeScaledDepthBias = 1.75f;
            shadowPipeline = pipelineFactory.createGraphicsPipeline(shadowDesc);
        }

        void requireFrameSlotReady(std::uint32_t frameIndex) const {
            if (frameIndex >= frameCount) {
                throw std::out_of_range("ModelRenderer frame index is out of range.");
            }
            frameSlotReadiness.requireReady(frameIndex);
        }

        [[nodiscard]] nvrhi::GraphicsState graphicsState(nvrhi::IFramebuffer& framebuffer,
                                                         nvrhi::IGraphicsPipeline& pipeline,
                                                         nvrhi::IBindingSet& bindingSet, std::uint32_t width,
                                                         std::uint32_t height) const {
            nvrhi::GraphicsState state;
            state.setPipeline(&pipeline)
                .setFramebuffer(&framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
                .addBindingSet(&bindingSet)
                .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(vertexBuffer).setSlot(0).setOffset(0))
                .setIndexBuffer(
                    nvrhi::IndexBufferBinding().setBuffer(indexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0))
                .setIndirectParams(indirectBuffer);
            return state;
        }
    };

    ModelBatch ModelRenderer::buildBatch(const world::RenderWorldSnapshot& worldSnapshot) {
        ModelBatch batch;
        const std::vector<scene::PbrTextureSet> textureSets = collectTextureSets(worldSnapshot);
        std::vector<MeshRange> meshRanges;
        meshRanges.reserve(worldSnapshot.meshes().size());

        for (const world::RenderWorldMesh& renderMesh : worldSnapshot.meshes()) {
            const assets::Mesh& mesh = renderMesh.mesh;
            const MeshRange range{
                checkedU32(batch.indices.size(), "Packed model indices exceed the uint32 range."),
                checkedU32(mesh.indices.size(), "A model has too many indices for an indirect draw."),
                checkedI32(batch.vertices.size(), "Packed model vertices exceed the int32 vertex offset."),
            };
            meshRanges.push_back(range);
            batch.vertices.insert(batch.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
            batch.indices.insert(batch.indices.end(), mesh.indices.begin(), mesh.indices.end());
        }

        batch.commands.reserve(worldSnapshot.instances().size());
        batch.objects.reserve(worldSnapshot.instances().size());
        std::size_t materialCount = 0;
        for (const world::RenderWorldInstance& instance : worldSnapshot.instances()) {
            const gpu::RenderInstanceId instanceId{instance.modelHandle};
            if (!instanceId.isValid()) {
                throw std::logic_error("A render-world instance has an invalid stable model handle.");
            }
            materialCount = std::max(materialCount, static_cast<std::size_t>(instanceId.slot()) + 1U);
        }
        batch.materials.resize(materialCount);
        for (const world::RenderWorldInstance& instance : worldSnapshot.instances()) {
            const scene::ModelInstance& model = instance.model;
            if (!model.mesh.isValid() || instance.meshIndex >= meshRanges.size()) {
                throw std::out_of_range("A render-world instance references an invalid compact mesh index.");
            }

            const MeshRange range = meshRanges[instance.meshIndex];
            batch.commands.push_back(ModelBatch::Command{
                .indexCount = range.indexCount,
                .instanceCount = 1,
                .startIndexLocation = range.firstIndex,
                .baseVertexLocation = range.vertexOffset,
                .startInstanceLocation = 0,
            });
            const glm::mat4 modelMatrix = model.transform.matrix();
            const gpu::GpuMaterialIndex materialIndex =
                gpu::materialIndexFor(gpu::RenderInstanceId{instance.modelHandle});
            const std::uint32_t textureDescriptorIndex = textureDescriptorIndexFor(model.material, textureSets);
            batch.materials[materialIndex.value()] = gpu::packGpuMaterial(model.material, textureDescriptorIndex);
            batch.objects.push_back(makeObjectData(model, modelMatrix, modelMatrix, textureSets, materialIndex));
        }

        if (batch.commands.size() != batch.objects.size()) {
            throw std::logic_error("Indirect command and object data counts diverged.");
        }
        return batch;
    }

    ModelRenderer::ModelRenderer(VulkanContext& context, const world::RenderWorldSnapshot& worldSnapshot,
                                 std::filesystem::path shaderDirectory, std::span<const nvrhi::Format> colorFormats,
                                 nvrhi::Format depthFormat, nvrhi::Format shadowDepthFormat, std::uint32_t frameCount,
                                 ModelRendererCapabilities capabilities)
        : impl_(std::make_unique<Impl>(context, worldSnapshot, std::move(shaderDirectory), colorFormats, depthFormat,
                                       shadowDepthFormat, frameCount, capabilities)) {
    }

    ModelRenderer::~ModelRenderer() = default;

    void ModelRenderer::sync(const world::RenderWorldSnapshot& worldSnapshot, std::uint32_t frameIndex,
                             bool resetMotion) {
        if (frameIndex >= impl_->frameCount) {
            throw std::out_of_range("ModelRenderer sync frame index is out of range.");
        }
        if (worldSnapshot.instances().size() != impl_->batch.objects.size()) {
            throw std::logic_error("Render-world topology changed without rebuilding ModelRenderer.");
        }

        impl_->hasPendingModels = false;
        for (std::size_t index = 0; index < worldSnapshot.instances().size(); ++index) {
            const world::RenderWorldInstance& instance = worldSnapshot.instances()[index];
            const scene::ModelInstance& model = instance.model;
            const gpu::GpuMaterialIndex materialIndex =
                gpu::materialIndexFor(gpu::RenderInstanceId{instance.modelHandle});
            if (!materialIndex.isValid() || materialIndex.value() >= impl_->batch.materials.size()) {
                throw std::logic_error("Render-world material slots changed without rebuilding ModelRenderer.");
            }
            const glm::mat4 currentModel = model.transform.matrix();
            const glm::mat4 previousModel = detail::requiresPreviousModelReset(resetMotion, impl_->hasPreviousModels)
                                                ? currentModel
                                                : impl_->previousModels[index];
            const std::uint32_t textureDescriptorIndex = textureDescriptorIndexFor(model.material, impl_->textureSets);
            impl_->batch.materials[materialIndex.value()] =
                gpu::packGpuMaterial(model.material, textureDescriptorIndex);
            impl_->batch.objects[index] =
                makeObjectData(model, currentModel, previousModel, impl_->textureSets, materialIndex);
            impl_->pendingModels[index] = currentModel;
        }
        impl_->writeCpuBuffer(impl_->objectBuffers[frameIndex], impl_->batch.objects.data(),
                              sizeof(ObjectData) * impl_->batch.objects.size());
        impl_->writeCpuBuffer(impl_->materialBuffers[frameIndex], impl_->batch.materials.data(),
                              sizeof(gpu::GpuMaterialData) * impl_->batch.materials.size());
        impl_->frameSlotReadiness.markReady(frameIndex);
        impl_->pendingFrameIndex = frameIndex;
        impl_->hasPendingModels = true;
    }

    void ModelRenderer::commitSubmittedFrame() noexcept {
        if (!impl_->hasPendingModels) {
            return;
        }
        impl_->previousModels.swap(impl_->pendingModels);
        impl_->hasPreviousModels = true;
        if (impl_->pendingFrameIndex < impl_->materialBufferInitialized.size()) {
            impl_->materialBufferInitialized[impl_->pendingFrameIndex] = true;
        }
        impl_->pendingFrameIndex = std::numeric_limits<std::uint32_t>::max();
        impl_->hasPendingModels = false;
    }

    void ModelRenderer::discardPendingFrame() noexcept {
        impl_->pendingFrameIndex = std::numeric_limits<std::uint32_t>::max();
        impl_->hasPendingModels = false;
    }

    void ModelRenderer::recordGBuffer(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                      std::uint32_t width, std::uint32_t height, std::uint32_t frameIndex,
                                      const glm::mat4& viewProjection, const glm::mat4& previousViewProjection) {
        impl_->requireFrameSlotReady(frameIndex);
        const Impl::FrameUniforms uniforms{viewProjection, previousViewProjection};
        impl_->writeCpuBuffer(impl_->uniformBuffers[frameIndex], &uniforms, sizeof(uniforms));
        const nvrhi::GraphicsState state = impl_->graphicsState(framebuffer, *impl_->gbufferPipeline,
                                                                *impl_->gbufferBindingSets[frameIndex], width, height);
        detail::recordModelIndexedIndirect(commandList, state, drawCount());
        impl_->frameSlotReadiness.consumeReady(frameIndex);
    }

    void ModelRenderer::recordShadow(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                     std::uint32_t width, std::uint32_t height, std::uint32_t frameIndex,
                                     std::uint32_t cascadeIndex, const glm::mat4& lightViewProjection) {
        impl_->requireFrameSlotReady(frameIndex);
        if (cascadeIndex >= cascadeCount) {
            throw std::out_of_range("ModelRenderer shadow cascade index is out of range.");
        }
        const std::uint32_t index = impl_->shadowIndex(frameIndex, cascadeIndex);
        const Impl::ShadowUniforms uniforms{lightViewProjection};
        impl_->writeCpuBuffer(impl_->shadowUniformBuffers[index], &uniforms, sizeof(uniforms));
        const nvrhi::GraphicsState state =
            impl_->graphicsState(framebuffer, *impl_->shadowPipeline, *impl_->shadowBindingSets[index], width, height);
        detail::recordModelIndexedIndirect(commandList, state, drawCount());
    }

    void ModelRenderer::record(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                               std::uint32_t frameIndex, const scene::Camera& camera, float aspectRatio) {
        const glm::mat4 viewProjection = camera.projectionMatrix(aspectRatio) * camera.viewMatrix();
        const nvrhi::FramebufferInfoEx& info = framebuffer.getFramebufferInfo();
        recordGBuffer(commandList, framebuffer, info.width, info.height, frameIndex, viewProjection, viewProjection);
    }

    std::uint32_t ModelRenderer::drawCount() const noexcept {
        return static_cast<std::uint32_t>(impl_->batch.commands.size());
    }

    const nvrhi::BufferHandle& ModelRenderer::vertexBuffer() const noexcept {
        return impl_->vertexBuffer;
    }

    const nvrhi::BufferHandle& ModelRenderer::indexBuffer() const noexcept {
        return impl_->indexBuffer;
    }

    const nvrhi::BufferHandle& ModelRenderer::indirectBuffer() const noexcept {
        return impl_->indirectBuffer;
    }

    const nvrhi::BufferHandle& ModelRenderer::objectBuffer(std::uint32_t frameIndex) const {
        if (frameIndex >= impl_->frameCount) {
            throw std::out_of_range("ModelRenderer object buffer frame index is out of range.");
        }
        return impl_->objectBuffers[frameIndex];
    }

    const nvrhi::BufferHandle& ModelRenderer::materialBuffer(std::uint32_t frameIndex) const {
        if (frameIndex >= impl_->materialBuffers.size()) {
            throw std::out_of_range("ModelRenderer material-buffer frame index is out of range.");
        }
        return impl_->materialBuffers[frameIndex];
    }

    nvrhi::ResourceStates ModelRenderer::materialBufferInitialState(std::uint32_t frameIndex) const {
        if (frameIndex >= impl_->materialBufferInitialized.size()) {
            throw std::out_of_range("ModelRenderer material-buffer state frame index is out of range.");
        }
        return impl_->materialBufferInitialized[frameIndex] ? nvrhi::ResourceStates::ShaderResource
                                                            : nvrhi::ResourceStates::Common;
    }

    const nvrhi::BufferHandle& ModelRenderer::frameUniformBuffer(std::uint32_t frameIndex) const {
        if (frameIndex >= impl_->frameCount) {
            throw std::out_of_range("ModelRenderer uniform buffer frame index is out of range.");
        }
        return impl_->uniformBuffers[frameIndex];
    }

    const nvrhi::BufferHandle& ModelRenderer::shadowUniformBuffer(std::uint32_t frameIndex,
                                                                  std::uint32_t cascadeIndex) const {
        if (frameIndex >= impl_->frameCount || cascadeIndex >= cascadeCount) {
            throw std::out_of_range("ModelRenderer shadow buffer index is out of range.");
        }
        return impl_->shadowUniformBuffers[impl_->shadowIndex(frameIndex, cascadeIndex)];
    }

    std::span<const nvrhi::TextureHandle> ModelRenderer::baseColorTextures() const noexcept {
        return impl_->baseColorTextures;
    }

    std::span<const nvrhi::TextureHandle> ModelRenderer::normalRoughnessTextures() const noexcept {
        return impl_->normalRoughnessTextures;
    }

    nvrhi::SamplerHandle ModelRenderer::materialSampler() const noexcept {
        return impl_->materialSampler;
    }

} // namespace lumin::render

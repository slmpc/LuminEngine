#include "render/gi/GiComposite.hpp"

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace lumin::render::gi {
    namespace {

        constexpr std::uint32_t diffuseBinding = 0;
        constexpr std::uint32_t specularBinding = 1;
        constexpr std::uint32_t positionBinding = 2;
        constexpr std::uint32_t normalRoughnessBinding = 3;
        constexpr std::uint32_t albedoMetallicBinding = 4;
        constexpr std::uint32_t materialIdBinding = 5;
        constexpr std::uint32_t materialsBinding = 6;
        constexpr std::uint32_t constantsBinding = 7;
        constexpr std::uint32_t outputBinding = 8;
        constexpr std::uint32_t threadGroupWidth = 8;
        constexpr std::uint32_t threadGroupHeight = 8;

        [[nodiscard]] std::uint32_t divideRoundUp(std::uint32_t value, std::uint32_t divisor) {
            if (value == 0 || divisor == 0) {
                throw std::invalid_argument("GI composite dispatch inputs must be non-zero.");
            }
            return value / divisor + (value % divisor == 0 ? 0U : 1U);
        }

        [[nodiscard]] bool isFloatFourChannel(nvrhi::Format format) noexcept {
            return format == nvrhi::Format::RGBA16_FLOAT || format == nvrhi::Format::RGBA32_FLOAT;
        }

        [[nodiscard]] bool isAlbedoFormat(nvrhi::Format format) noexcept {
            return format == nvrhi::Format::RGBA8_UNORM || format == nvrhi::Format::BGRA8_UNORM ||
                   isFloatFourChannel(format);
        }

        template <typename FormatPredicate>
        void validateTexture(nvrhi::ITexture* texture, core::RenderExtent extent, bool requireUav,
                             FormatPredicate&& acceptsFormat, const char* label) {
            if (texture == nullptr) {
                throw std::invalid_argument(std::string{"GI composite is missing "} + label + ".");
            }
            const nvrhi::TextureDesc& desc = texture->getDesc();
            if (desc.width != extent.width || desc.height != extent.height ||
                desc.dimension != nvrhi::TextureDimension::Texture2D || !acceptsFormat(desc.format) ||
                !desc.isShaderResource || (requireUav && !desc.isUAV)) {
                throw std::invalid_argument(std::string{"GI composite texture contract mismatch: "} + label + ".");
            }
        }

        [[nodiscard]] bool complete(const GiCompositeGraphResources& resources) noexcept {
            return resources.diffuseRadianceHitDistance.isValid() && resources.specularRadianceHitDistance.isValid() &&
                   resources.position.isValid() && resources.normalRoughness.isValid() &&
                   resources.albedoMetallic.isValid() && resources.materialId.isValid() &&
                   resources.materials.isValid() && resources.globalIllumination.isValid();
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer, const GiCompositeConstants& constants) {
            if (buffer == nullptr) {
                throw std::invalid_argument("GI composite constants buffer is null.");
            }
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map GI composite constants buffer.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

        [[nodiscard]] float finiteSaturate(float value) noexcept {
            return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
        }

        [[nodiscard]] glm::vec3 finiteSaturate(const glm::vec3& value) noexcept {
            return {finiteSaturate(value.x), finiteSaturate(value.y), finiteSaturate(value.z)};
        }

        [[nodiscard]] glm::vec3 finitePositive(const glm::vec3& value) noexcept {
            return {
                std::isfinite(value.x) ? std::max(value.x, 0.0F) : 0.0F,
                std::isfinite(value.y) ? std::max(value.y, 0.0F) : 0.0F,
                std::isfinite(value.z) ? std::max(value.z, 0.0F) : 0.0F,
            };
        }

        [[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback) noexcept {
            const float lengthSquared = glm::dot(value, value);
            if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) {
                return fallback;
            }
            return value / std::sqrt(lengthSquared);
        }

    } // namespace

    namespace detail {

        nvrhi::BufferDesc makeGiCompositeConstantBufferDesc() {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(GiCompositeConstants);
            desc.debugName = "GI composite constants";
            desc.isConstantBuffer = true;
            desc.cpuAccess = nvrhi::CpuAccessMode::Write;
            // 常量缓冲整个生命周期只被 compute shader 读取，不需要跨帧提交状态事务。
            desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
            desc.keepInitialState = false;
            return desc;
        }

        nvrhi::BindingLayoutDesc makeGiCompositeBindingLayoutDesc() {
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                       .setShaderResourceOffset(0)
                                       .setSamplerOffset(0)
                                       .setConstantBufferOffset(0)
                                       .setUnorderedAccessViewOffset(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(diffuseBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(specularBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(positionBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(normalRoughnessBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(albedoMetallicBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(materialIdBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(materialsBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(constantsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(outputBinding));
            return desc;
        }

        nvrhi::BindingSetDesc makeGiCompositeBindingSetDesc(const GiCompositeResources& resources,
                                                            nvrhi::IBuffer* constants) {
            if (!resources.diffuseRadianceHitDistance || !resources.specularRadianceHitDistance ||
                !resources.position || !resources.normalRoughness || !resources.albedoMetallic ||
                !resources.materialId || !resources.materials || !resources.globalIllumination ||
                constants == nullptr) {
                throw std::invalid_argument("GI composite binding set requires complete native resources.");
            }

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(diffuseBinding, resources.diffuseRadianceHitDistance))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(specularBinding, resources.specularRadianceHitDistance))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(positionBinding, resources.position))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(normalRoughnessBinding, resources.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(albedoMetallicBinding, resources.albedoMetallic))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(materialIdBinding, resources.materialId))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(materialsBinding, resources.materials))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(constantsBinding, constants))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(outputBinding, resources.globalIllumination));
            return desc;
        }

        std::uint32_t validateGiCompositeResources(const GiCompositeResources& resources, core::RenderExtent extent) {
            if (extent.isEmpty()) {
                throw std::invalid_argument("GI composite resources require a non-empty extent.");
            }
            validateTexture(resources.diffuseRadianceHitDistance, extent, false, isFloatFourChannel,
                            "diffuse NRD output");
            validateTexture(resources.specularRadianceHitDistance, extent, false, isFloatFourChannel,
                            "specular NRD output");
            validateTexture(resources.position, extent, false, isFloatFourChannel, "G-buffer position");
            validateTexture(resources.normalRoughness, extent, false, isFloatFourChannel, "G-buffer normal/roughness");
            validateTexture(resources.albedoMetallic, extent, false, isAlbedoFormat, "G-buffer albedo/metallic");
            validateTexture(
                resources.materialId, extent, false,
                [](nvrhi::Format format) {
                    return format == nvrhi::Format::R32_UINT;
                },
                "G-buffer material ID");
            validateTexture(resources.globalIllumination, extent, true, isFloatFourChannel,
                            "global-illumination output");

            if (!resources.materials) {
                throw std::invalid_argument("GI composite is missing the material buffer.");
            }
            const nvrhi::BufferDesc& desc = resources.materials->getDesc();
            if (desc.structStride != sizeof(gpu::GpuMaterialData) || desc.byteSize == 0 ||
                desc.byteSize % sizeof(gpu::GpuMaterialData) != 0) {
                throw std::invalid_argument("GI composite material buffer does not use GpuMaterialData stride.");
            }
            const std::uint64_t materialCount = desc.byteSize / sizeof(gpu::GpuMaterialData);
            if (materialCount > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("GI composite material table exceeds the shader index range.");
            }
            return static_cast<std::uint32_t>(materialCount);
        }

        GiCompositeDispatchSize makeGiCompositeDispatchSize(core::RenderExtent extent) {
            if (extent.isEmpty()) {
                throw std::invalid_argument("GI composite dispatch requires a non-empty extent.");
            }
            return {divideRoundUp(extent.width, threadGroupWidth), divideRoundUp(extent.height, threadGroupHeight), 1};
        }

        GiCompositeConstants makeGiCompositeConstants(const GiCompositeFrameParameters& parameters,
                                                      std::uint32_t materialCount) {
            if (parameters.extent.isEmpty() || materialCount == 0 || !std::isfinite(parameters.cameraPosition.x) ||
                !std::isfinite(parameters.cameraPosition.y) || !std::isfinite(parameters.cameraPosition.z)) {
                throw std::invalid_argument(
                    "GI composite constants require extent, materials, and finite camera data.");
            }
            GiCompositeConstants result;
            result.cameraPosition = glm::vec4{parameters.cameraPosition, 1.0F};
            result.renderInfo = glm::uvec4{parameters.extent.width, parameters.extent.height, materialCount,
                                           std::numeric_limits<std::uint32_t>::max()};
            return result;
        }

        glm::vec3 modulateGiRadiance(const glm::vec3& diffuseRadiance, const glm::vec3& specularRadiance,
                                     const glm::vec3& albedo, float metallic, const glm::vec3& normal,
                                     const glm::vec3& toView, const gpu::GpuMaterialData& material) noexcept {
            const glm::vec3 diffuse = finitePositive(diffuseRadiance);
            const glm::vec3 specular = finitePositive(specularRadiance);
            const glm::vec3 baseColor = finiteSaturate(albedo);
            if (material.metadata.x == static_cast<std::uint32_t>(scene::SurfaceModel::BlinnPhong)) {
                return diffuse * baseColor + specular * finiteSaturate(glm::vec3{material.specularColorShininess});
            }

            const glm::vec3 surfaceNormal = safeNormalize(normal, glm::vec3{0.0F, 0.0F, 1.0F});
            const glm::vec3 viewDirection = safeNormalize(toView, surfaceNormal);
            const float normalDotView = finiteSaturate(glm::dot(surfaceNormal, viewDirection));
            const float clampedMetallic = finiteSaturate(metallic);
            const glm::vec3 reflectanceAtNormal = glm::mix(glm::vec3{0.04F}, baseColor, clampedMetallic);
            const float fresnelPower = std::pow(1.0F - normalDotView, 5.0F);
            const glm::vec3 fresnel = reflectanceAtNormal + (glm::vec3{1.0F} - reflectanceAtNormal) * fresnelPower;
            const glm::vec3 diffuseWeight = (glm::vec3{1.0F} - fresnel) * (1.0F - clampedMetallic);
            return diffuse * baseColor * diffuseWeight + specular * fresnel;
        }

        FrameGraphPassHandle addGiCompositePass(FrameGraph& frameGraph, const GiCompositeGraphResources& resources,
                                                FrameGraphResourceHandle constants, FrameGraphPassHandle dependency,
                                                FrameGraph::ExecuteCallback execute) {
            if (!complete(resources) || !constants.isValid() || !execute) {
                throw std::invalid_argument("GI composite pass requires complete graph resources and execution.");
            }
            return frameGraph.addPass(
                "gi-composite", FrameGraphPassType::Compute,
                [resources, constants, dependency](FrameGraphBuilder& builder) {
                    if (dependency.isValid()) {
                        builder.dependsOn(dependency);
                    }
                    builder.readTexture(resources.diffuseRadianceHitDistance, nvrhi::ResourceStates::ShaderResource);
                    builder.readTexture(resources.specularRadianceHitDistance, nvrhi::ResourceStates::ShaderResource);
                    builder.readTexture(resources.position, nvrhi::ResourceStates::ShaderResource);
                    builder.readTexture(resources.normalRoughness, nvrhi::ResourceStates::ShaderResource);
                    builder.readTexture(resources.albedoMetallic, nvrhi::ResourceStates::ShaderResource);
                    builder.readTexture(resources.materialId, nvrhi::ResourceStates::ShaderResource);
                    builder.read(resources.materials, nvrhi::ResourceStates::ShaderResource);
                    builder.read(constants, nvrhi::ResourceStates::ConstantBuffer);
                    builder.writeTexture(resources.globalIllumination, nvrhi::ResourceStates::UnorderedAccess);
                },
                std::move(execute));
        }

    } // namespace detail

    struct GiCompositePass::Impl {
        nvrhi::IDevice& device;
        core::RenderExtent extent;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::ComputePipelineHandle pipeline;
        std::vector<nvrhi::BufferHandle> constants;
        std::vector<nvrhi::BindingSetHandle> bindingSets;

        explicit Impl(const GiCompositeCreateInfo& createInfo)
            : device(*createInfo.device), extent(createInfo.extent), bindingSets(createInfo.frameSlotCount) {
            bindingLayout = device.createBindingLayout(detail::makeGiCompositeBindingLayoutDesc());
            if (!bindingLayout) {
                throw std::runtime_error("Failed to create the GI composite binding layout.");
            }

            ShaderLibrary shaders(device, createInfo.shaderDirectory);
            PipelineFactory pipelineFactory(device);
            const nvrhi::ShaderHandle compute = shaders.loadComputeModule("gi_composite.comp.spv", "compositeMain");
            const std::array layouts = {bindingLayout};
            pipeline = pipelineFactory.createComputePipeline({compute, layouts});

            constants.reserve(createInfo.frameSlotCount);
            for (std::uint32_t frameSlot = 0; frameSlot < createInfo.frameSlotCount; ++frameSlot) {
                nvrhi::BufferHandle buffer = device.createBuffer(detail::makeGiCompositeConstantBufferDesc());
                if (!buffer) {
                    throw std::runtime_error("Failed to create a GI composite constants buffer.");
                }
                constants.push_back(std::move(buffer));
            }
        }
    };

    GiCompositePass::GiCompositePass(const GiCompositeCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaderDirectory.empty() || createInfo.extent.isEmpty() ||
            createInfo.frameSlotCount == 0) {
            throw std::invalid_argument("GI composite requires a device, shader directory, extent, and frame slots.");
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    GiCompositePass::~GiCompositePass() = default;

    FrameGraphPassHandle GiCompositePass::record(FrameGraph& frameGraph, const GiCompositeFrameParameters& parameters,
                                                 const GiCompositeResources& resources,
                                                 const GiCompositeGraphResources& graphResources,
                                                 FrameGraphPassHandle dependency) {
        if (!parameters.frameSlot.isValid() || parameters.frameSlot.value() >= impl_->constants.size()) {
            throw std::out_of_range("GI composite frame slot is outside the configured range.");
        }
        if (!parameters.frameSlotFenceWaited) {
            throw std::logic_error("GI composite frame-slot resources may only change after waiting for its fence.");
        }
        if (parameters.extent != impl_->extent) {
            throw std::invalid_argument("GI composite frame extent differs from its created resources.");
        }
        if (!complete(graphResources)) {
            throw std::invalid_argument("GI composite requires existing FrameGraph identities for every resource.");
        }

        const std::uint32_t materialCount = detail::validateGiCompositeResources(resources, parameters.extent);
        const GiCompositeConstants constants = detail::makeGiCompositeConstants(parameters, materialCount);
        const std::uint32_t frameSlot = parameters.frameSlot.value();
        writeConstants(impl_->device, impl_->constants[frameSlot], constants);

        nvrhi::BindingSetHandle bindingSet = impl_->device.createBindingSet(
            detail::makeGiCompositeBindingSetDesc(resources, impl_->constants[frameSlot]), impl_->bindingLayout);
        if (!bindingSet) {
            throw std::runtime_error("Failed to create the GI composite binding set.");
        }
        impl_->bindingSets[frameSlot] = bindingSet;

        const FrameGraphResourceHandle constantsResource =
            frameGraph.importBuffer("gi-composite.constants." + std::to_string(frameSlot),
                                    FrameGraphBufferDesc{
                                        .size = sizeof(GiCompositeConstants),
                                        .buffer = impl_->constants[frameSlot],
                                        .initialState = nvrhi::ResourceStates::ConstantBuffer,
                                        .finalState = nvrhi::ResourceStates::ConstantBuffer,
                                    });
        const nvrhi::ComputePipelineHandle pipeline = impl_->pipeline;
        const GiCompositeDispatchSize dispatch = detail::makeGiCompositeDispatchSize(parameters.extent);
        return detail::addGiCompositePass(frameGraph, graphResources, constantsResource, dependency,
                                          [pipeline, bindingSet, dispatch](const FrameGraphContext& context) {
                                              if (context.commandList == nullptr) {
                                                  throw std::logic_error(
                                                      "GI composite dispatch requires an NvRHI command list.");
                                              }
                                              nvrhi::ComputeState state;
                                              state.setPipeline(pipeline).addBindingSet(bindingSet);
                                              detail::recordGiCompositeDispatch(*context.commandList, state, dispatch);
                                          });
    }

} // namespace lumin::render::gi

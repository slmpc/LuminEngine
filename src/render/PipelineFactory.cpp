#include "render/PipelineFactory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace lumin::render {
    namespace {

        [[nodiscard]] bool hasEmbeddedNull(std::string_view value) noexcept {
            return value.find('\0') != std::string_view::npos;
        }

        void validateExportName(std::string_view exportName) {
            if (exportName.empty() || hasEmbeddedNull(exportName)) {
                throw std::invalid_argument("Ray tracing export names must be non-empty and cannot contain NUL.");
            }
        }

        void validateBindingLayouts(std::span<const nvrhi::BindingLayoutHandle> bindingLayouts,
                                    std::string_view pipelineType) {
            if (bindingLayouts.size() > nvrhi::c_MaxBindingLayouts) {
                throw std::invalid_argument("Too many binding layouts for " + std::string(pipelineType) + " pipeline.");
            }
            if (std::ranges::any_of(bindingLayouts, [](const nvrhi::BindingLayoutHandle& layout) {
                    return !layout;
                })) {
                throw std::invalid_argument("Pipeline binding layouts cannot contain null handles.");
            }
        }

        [[nodiscard]] nvrhi::ShaderType shaderType(const nvrhi::ShaderHandle& shader, std::string_view role) {
            if (!shader) {
                throw std::invalid_argument(std::string(role) + " shader cannot be null.");
            }
            return shader->getDesc().shaderType;
        }

        [[nodiscard]] bool isGeneralRayTracingShader(nvrhi::ShaderType type) noexcept {
            return type == nvrhi::ShaderType::RayGeneration || type == nvrhi::ShaderType::Miss ||
                   type == nvrhi::ShaderType::Callable;
        }

        void requireShaderType(const nvrhi::ShaderHandle& shader, nvrhi::ShaderType expected, std::string_view role) {
            if (shaderType(shader, role) != expected) {
                throw std::invalid_argument(std::string(role) + " shader has an incompatible shader stage.");
            }
        }

        [[nodiscard]] std::uint32_t shaderTableEntryCount(const RayTracingShaderTableDesc& desc) {
            std::size_t entryCount = 1;
            for (const std::size_t additionalEntries :
                 {desc.missShaders.size(), desc.hitGroups.size(), desc.callableShaders.size()}) {
                if (additionalEntries > std::numeric_limits<std::uint32_t>::max() - entryCount) {
                    throw std::invalid_argument("Ray tracing shader table has too many entries for NvRHI.");
                }
                entryCount += additionalEntries;
            }
            return static_cast<std::uint32_t>(entryCount);
        }

        void validateShaderTableShape(const RayTracingShaderTableDesc& desc) {
            validateExportName(desc.rayGenerationExport);
            const auto validateEntries = [](std::span<const RayTracingShaderTableEntryDesc> entries) {
                for (const RayTracingShaderTableEntryDesc& entry : entries) {
                    validateExportName(entry.exportName);
                }
            };
            validateEntries(desc.missShaders);
            validateEntries(desc.hitGroups);
            validateEntries(desc.callableShaders);

            const std::uint32_t entryCount = shaderTableEntryCount(desc);
            if (desc.cached) {
                if (desc.maxEntries == 0 || entryCount > desc.maxEntries) {
                    throw std::invalid_argument(
                        "Cached ray tracing shader table capacity must cover every table entry.");
                }
            } else if (desc.maxEntries != 0) {
                throw std::invalid_argument("Uncached ray tracing shader tables must use zero maxEntries.");
            }
        }

        [[nodiscard]] bool pipelineHasShaderExport(const nvrhi::rt::PipelineDesc& pipelineDesc,
                                                   std::string_view exportName, nvrhi::ShaderType expectedType) {
            return std::ranges::any_of(pipelineDesc.shaders, [exportName, expectedType](const auto& shader) {
                return shader.exportName == exportName && shader.shader &&
                       shader.shader->getDesc().shaderType == expectedType;
            });
        }

        [[nodiscard]] bool pipelineHasHitGroupExport(const nvrhi::rt::PipelineDesc& pipelineDesc,
                                                     std::string_view exportName) {
            return std::ranges::any_of(pipelineDesc.hitGroups, [exportName](const auto& hitGroup) {
                return hitGroup.exportName == exportName;
            });
        }

    } // namespace

    namespace detail {

        nvrhi::ComputePipelineDesc makeComputePipelineDesc(const ComputePipelineDesc& desc) {
            requireShaderType(desc.computeShader, nvrhi::ShaderType::Compute, "Compute pipeline");
            validateBindingLayouts(desc.bindingLayouts, "compute");

            nvrhi::ComputePipelineDesc pipelineDesc;
            pipelineDesc.setComputeShader(desc.computeShader);
            for (const nvrhi::BindingLayoutHandle& bindingLayout : desc.bindingLayouts) {
                pipelineDesc.addBindingLayout(bindingLayout);
            }
            return pipelineDesc;
        }

        nvrhi::rt::PipelineDesc makeRayTracingPipelineDesc(const RayTracingPipelineDesc& desc) {
            validateBindingLayouts(desc.globalBindingLayouts, "ray tracing");
            if (desc.maxRecursionDepth == 0) {
                throw std::invalid_argument("Ray tracing pipeline recursion depth must be greater than zero.");
            }
            if (desc.hlslExtensionsUAV < -1) {
                throw std::invalid_argument("Ray tracing HLSL extension UAV must be -1 or non-negative.");
            }

            std::unordered_set<std::string_view> exports;
            bool hasRayGenerationShader = false;
            nvrhi::rt::PipelineDesc pipelineDesc;
            for (const RayTracingPipelineShaderDesc& shader : desc.shaders) {
                validateExportName(shader.exportName);
                if (!exports.emplace(shader.exportName).second) {
                    throw std::invalid_argument("Ray tracing pipeline export names must be unique.");
                }
                const nvrhi::ShaderType type = shaderType(shader.shader, "Ray tracing pipeline");
                if (!isGeneralRayTracingShader(type)) {
                    throw std::invalid_argument(
                        "General ray tracing exports must be ray-generation, miss, or callable shaders.");
                }
                hasRayGenerationShader |= type == nvrhi::ShaderType::RayGeneration;

                nvrhi::rt::PipelineShaderDesc shaderDesc;
                shaderDesc.setExportName(shader.exportName).setShader(shader.shader);
                pipelineDesc.addShader(shaderDesc);
            }
            if (!hasRayGenerationShader) {
                throw std::invalid_argument("Ray tracing pipeline requires at least one ray-generation shader.");
            }

            for (const RayTracingHitGroupDesc& hitGroup : desc.hitGroups) {
                validateExportName(hitGroup.exportName);
                if (!exports.emplace(hitGroup.exportName).second) {
                    throw std::invalid_argument("Ray tracing pipeline export names must be unique.");
                }
                if (!hitGroup.closestHitShader && !hitGroup.anyHitShader && !hitGroup.intersectionShader) {
                    throw std::invalid_argument("Ray tracing hit group must contain at least one shader.");
                }
                if (hitGroup.closestHitShader) {
                    requireShaderType(hitGroup.closestHitShader, nvrhi::ShaderType::ClosestHit, "Closest-hit");
                }
                if (hitGroup.anyHitShader) {
                    requireShaderType(hitGroup.anyHitShader, nvrhi::ShaderType::AnyHit, "Any-hit");
                }
                if (hitGroup.proceduralPrimitive) {
                    requireShaderType(hitGroup.intersectionShader, nvrhi::ShaderType::Intersection, "Intersection");
                } else if (hitGroup.intersectionShader) {
                    throw std::invalid_argument("Triangle hit groups cannot contain an intersection shader.");
                }

                nvrhi::rt::PipelineHitGroupDesc hitGroupDesc;
                hitGroupDesc.setExportName(hitGroup.exportName)
                    .setClosestHitShader(hitGroup.closestHitShader)
                    .setAnyHitShader(hitGroup.anyHitShader)
                    .setIntersectionShader(hitGroup.intersectionShader)
                    .setIsProceduralPrimitive(hitGroup.proceduralPrimitive);
                pipelineDesc.addHitGroup(hitGroupDesc);
            }
            for (const nvrhi::BindingLayoutHandle& bindingLayout : desc.globalBindingLayouts) {
                pipelineDesc.addBindingLayout(bindingLayout);
            }
            pipelineDesc.setMaxPayloadSize(desc.maxPayloadSize)
                .setMaxAttributeSize(desc.maxAttributeSize)
                .setMaxRecursionDepth(desc.maxRecursionDepth)
                .setHlslExtensionsUAV(desc.hlslExtensionsUAV)
                .setAllowOpacityMicromaps(desc.allowOpacityMicromaps);
            return pipelineDesc;
        }

        nvrhi::rt::ShaderTableDesc makeRayTracingShaderTableDesc(const RayTracingShaderTableDesc& desc) {
            validateShaderTableShape(desc);
            nvrhi::rt::ShaderTableDesc tableDesc;
            tableDesc.setIsCached(desc.cached).setMaxEntries(desc.maxEntries).setDebugName(desc.debugName);
            return tableDesc;
        }

        void validateRayTracingShaderTable(const nvrhi::rt::PipelineDesc& pipelineDesc,
                                           const RayTracingShaderTableDesc& tableDesc) {
            validateShaderTableShape(tableDesc);
            if (!pipelineHasShaderExport(pipelineDesc, tableDesc.rayGenerationExport,
                                         nvrhi::ShaderType::RayGeneration)) {
                throw std::invalid_argument("Shader table ray-generation export is missing from the pipeline.");
            }
            for (const RayTracingShaderTableEntryDesc& entry : tableDesc.missShaders) {
                if (!pipelineHasShaderExport(pipelineDesc, entry.exportName, nvrhi::ShaderType::Miss)) {
                    throw std::invalid_argument("Shader table miss export is missing from the pipeline.");
                }
            }
            for (const RayTracingShaderTableEntryDesc& entry : tableDesc.hitGroups) {
                if (!pipelineHasHitGroupExport(pipelineDesc, entry.exportName)) {
                    throw std::invalid_argument("Shader table hit-group export is missing from the pipeline.");
                }
            }
            for (const RayTracingShaderTableEntryDesc& entry : tableDesc.callableShaders) {
                if (!pipelineHasShaderExport(pipelineDesc, entry.exportName, nvrhi::ShaderType::Callable)) {
                    throw std::invalid_argument("Shader table callable export is missing from the pipeline.");
                }
            }
        }

    } // namespace detail

    PipelineFactory::PipelineFactory(nvrhi::IDevice& device) : device_(device) {
    }

    nvrhi::GraphicsPipelineHandle PipelineFactory::createGraphicsPipeline(const GraphicsPipelineDesc& desc) const {
        if (desc.bindingLayouts.size() > nvrhi::c_MaxBindingLayouts) {
            throw std::invalid_argument("Too many binding layouts for graphics pipeline.");
        }
        if (desc.colorFormats.size() > nvrhi::c_MaxRenderTargets) {
            throw std::invalid_argument("Too many color formats for graphics pipeline.");
        }

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setInputLayout(desc.inputLayout)
            .setVertexShader(desc.vertexShader)
            .setFragmentShader(desc.fragmentShader);
        for (const nvrhi::BindingLayoutHandle& bindingLayout : desc.bindingLayouts) {
            pipelineDesc.addBindingLayout(bindingLayout);
        }

        pipelineDesc.renderState.rasterState.setFillSolid()
            .setCullMode(desc.cullMode)
            .setFrontCounterClockwise(true)
            .setDepthBias(desc.depthBiasEnable ? desc.depthBias : 0)
            .setSlopeScaleDepthBias(desc.depthBiasEnable ? desc.slopeScaledDepthBias : 0.0f);
        pipelineDesc.renderState.depthStencilState.setDepthTestEnable(desc.depthTestEnable)
            .setDepthWriteEnable(desc.depthWriteEnable)
            .setDepthFunc(desc.depthCompareOp);

        nvrhi::FramebufferInfo framebufferInfo;
        for (nvrhi::Format colorFormat : desc.colorFormats) {
            framebufferInfo.addColorFormat(colorFormat);
        }
        framebufferInfo.setDepthFormat(desc.depthFormat).setSampleCount(desc.sampleCount);

        nvrhi::GraphicsPipelineHandle pipeline = device_.createGraphicsPipeline(pipelineDesc, framebufferInfo);
        if (!pipeline) {
            throw std::runtime_error("Failed to create graphics pipeline.");
        }
        return pipeline;
    }

    nvrhi::ComputePipelineHandle PipelineFactory::createComputePipeline(const ComputePipelineDesc& desc) const {
        const nvrhi::ComputePipelineDesc pipelineDesc = detail::makeComputePipelineDesc(desc);
        nvrhi::ComputePipelineHandle pipeline = device_.createComputePipeline(pipelineDesc);
        if (!pipeline) {
            throw std::runtime_error("Failed to create compute pipeline.");
        }
        return pipeline;
    }

    nvrhi::rt::PipelineHandle PipelineFactory::createRayTracingPipeline(const RayTracingPipelineDesc& desc) const {
        const nvrhi::rt::PipelineDesc pipelineDesc = detail::makeRayTracingPipelineDesc(desc);
        nvrhi::rt::PipelineHandle pipeline = device_.createRayTracingPipeline(pipelineDesc);
        if (!pipeline) {
            throw std::runtime_error("Failed to create ray tracing pipeline.");
        }
        return pipeline;
    }

    nvrhi::rt::ShaderTableHandle
    PipelineFactory::createRayTracingShaderTable(const nvrhi::rt::PipelineHandle& pipeline,
                                                 const RayTracingShaderTableDesc& desc) const {
        if (!pipeline) {
            throw std::invalid_argument("Ray tracing shader table requires a valid pipeline.");
        }
        detail::validateRayTracingShaderTable(pipeline->getDesc(), desc);
        const nvrhi::rt::ShaderTableDesc tableDesc = detail::makeRayTracingShaderTableDesc(desc);
        nvrhi::rt::ShaderTableHandle shaderTable = pipeline->createShaderTable(tableDesc);
        if (!shaderTable) {
            throw std::runtime_error("Failed to create ray tracing shader table.");
        }

        shaderTable->setRayGenerationShader(desc.rayGenerationExport.c_str());
        for (const RayTracingShaderTableEntryDesc& entry : desc.missShaders) {
            if (shaderTable->addMissShader(entry.exportName.c_str()) < 0) {
                throw std::runtime_error("Failed to add a miss shader table entry.");
            }
        }
        for (const RayTracingShaderTableEntryDesc& entry : desc.hitGroups) {
            if (shaderTable->addHitGroup(entry.exportName.c_str()) < 0) {
                throw std::runtime_error("Failed to add a hit-group shader table entry.");
            }
        }
        for (const RayTracingShaderTableEntryDesc& entry : desc.callableShaders) {
            if (shaderTable->addCallableShader(entry.exportName.c_str()) < 0) {
                throw std::runtime_error("Failed to add a callable shader table entry.");
            }
        }
        return shaderTable;
    }

} // namespace lumin::render

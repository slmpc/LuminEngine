#include "render/gi/SharcRadianceCache.hpp"

#include "render/PipelineFactory.hpp"
#include "render/ShaderLibrary.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lumin::render::gi {
    namespace {

        constexpr std::uint32_t updateTlasBinding = 0;
        constexpr std::uint32_t updatePositionBinding = 1;
        constexpr std::uint32_t updateNormalRoughnessBinding = 2;
        constexpr std::uint32_t updateAlbedoMetallicBinding = 3;
        constexpr std::uint32_t updateVertexBuffersBinding = 4;
        constexpr std::uint32_t updateIndexBuffersBinding = 5;
        constexpr std::uint32_t updateInstancesBinding = 6;
        constexpr std::uint32_t updateMaterialsBinding = 7;
        constexpr std::uint32_t updateHashEntriesBinding = 8;
        constexpr std::uint32_t updateAccumulationBinding = 9;
        constexpr std::uint32_t updateResolvedBinding = 10;
        constexpr std::uint32_t updateLockBinding = 11;
        constexpr std::uint32_t updateStatisticsBinding = 12;
        constexpr std::uint32_t updateConstantsBinding = 13;
        constexpr std::uint32_t updateBaseColorTexturesBinding = 14;
        constexpr std::uint32_t updateNormalRoughnessTexturesBinding = 15;
        constexpr std::uint32_t updateMaterialSamplerBinding = 16;

        constexpr std::uint32_t resolveHashEntriesBinding = 0;
        constexpr std::uint32_t resolveAccumulationBinding = 1;
        constexpr std::uint32_t resolveResolvedBinding = 2;
        constexpr std::uint32_t resolveLockBinding = 3;
        constexpr std::uint32_t resolveStatisticsBinding = 4;
        constexpr std::uint32_t resolveConstantsBinding = 5;

        [[nodiscard]] bool finite(float value) noexcept {
            return std::isfinite(value);
        }

        [[nodiscard]] bool finite(const glm::vec4& value) noexcept {
            return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
        }

        [[nodiscard]] bool isPowerOfTwo(std::uint32_t value) noexcept {
            return value != 0 && (value & (value - 1U)) == 0;
        }

        [[nodiscard]] bool complete(const SharcUpdateFrameInputs& inputs) noexcept {
            return inputs.position && inputs.normalRoughness && inputs.albedoMetallic;
        }

        [[nodiscard]] bool complete(const SharcUpdateSceneBindings& scene) noexcept {
            return scene.descriptors.rayTracingEnabled && scene.descriptors.tlas && scene.descriptors.instances &&
                   scene.descriptors.materials && !scene.geometry.empty() && !scene.baseColorTextures.empty() &&
                   scene.baseColorTextures.size() == scene.normalRoughnessTextures.size() && scene.materialSampler;
        }

        [[nodiscard]] std::uint64_t byteSize(std::uint32_t capacity, std::uint32_t stride) {
            if (capacity == 0 || stride == 0 || capacity > std::numeric_limits<std::uint64_t>::max() / stride) {
                throw std::overflow_error("SHARC buffer byte size overflowed uint64.");
            }
            return static_cast<std::uint64_t>(capacity) * stride;
        }

        void writeConstants(nvrhi::IDevice& device, nvrhi::IBuffer* buffer, const SharcGpuConstants& constants) {
            if (buffer == nullptr) {
                throw std::invalid_argument("SHARC constants buffer is null.");
            }
            void* mapped = device.mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map SHARC constants buffer.");
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            device.unmapBuffer(buffer);
        }

        [[nodiscard]] nvrhi::BindingSetDesc makeUpdateBindingSetDesc(const SharcUpdateFrameInputs& inputs,
                                                                     const SharcUpdateSceneBindings& scene,
                                                                     const SharcNativeResources& resources,
                                                                     std::uint32_t maxGeometryDescriptors,
                                                                     std::uint32_t maxMaterialTextureDescriptors) {
            if (!complete(inputs) || !complete(scene) || !resources.isValid() ||
                scene.geometry.size() > maxGeometryDescriptors ||
                scene.baseColorTextures.size() > maxMaterialTextureDescriptors) {
                throw std::invalid_argument(
                    "SHARC update binding set requires complete G-buffer, scene, and cache resources.");
            }

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(updateTlasBinding, scene.descriptors.tlas))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(updatePositionBinding, inputs.position))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(updateNormalRoughnessBinding, inputs.normalRoughness))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(updateAlbedoMetallicBinding, inputs.albedoMetallic))
                .addItem(
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(updateInstancesBinding, scene.descriptors.instances))
                .addItem(
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(updateMaterialsBinding, scene.descriptors.materials))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(updateHashEntriesBinding, resources.hashEntries))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(updateAccumulationBinding, resources.accumulation))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(updateResolvedBinding, resources.resolved))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(updateLockBinding, resources.lock))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(updateStatisticsBinding, resources.statistics))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(updateConstantsBinding, resources.constants));
            for (std::uint32_t index = 0; index < maxGeometryDescriptors; ++index) {
                // 普通 NvRHI descriptor 数组要求完整写入；多余槽位复用任一有效 geometry。
                const gpu::GpuGeometryDescriptor& geometry = scene.geometry[index % scene.geometry.size()];
                if (!geometry.vertices || !geometry.indices) {
                    throw std::invalid_argument("SHARC update bindless geometry contains an incomplete descriptor.");
                }
                desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(updateVertexBuffersBinding, geometry.vertices)
                                 .setArrayElement(index));
                desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(updateIndexBuffersBinding, geometry.indices)
                                 .setArrayElement(index));
            }
            for (std::uint32_t index = 0; index < maxMaterialTextureDescriptors; ++index) {
                const std::size_t textureIndex = index % scene.baseColorTextures.size();
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(updateBaseColorTexturesBinding,
                                                                scene.baseColorTextures[textureIndex])
                                 .setArrayElement(index));
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(updateNormalRoughnessTexturesBinding,
                                                                scene.normalRoughnessTextures[textureIndex])
                                 .setArrayElement(index));
            }
            desc.addItem(nvrhi::BindingSetItem::Sampler(updateMaterialSamplerBinding, scene.materialSampler));
            return desc;
        }

        [[nodiscard]] nvrhi::BindingSetDesc makeResolveBindingSetDesc(const SharcNativeResources& resources) {
            if (!resources.isValid()) {
                throw std::invalid_argument("SHARC resolve binding set requires complete cache resources.");
            }
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(resolveHashEntriesBinding, resources.hashEntries))
                .addItem(
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(resolveAccumulationBinding, resources.accumulation))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(resolveResolvedBinding, resources.resolved))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(resolveLockBinding, resources.lock))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(resolveStatisticsBinding, resources.statistics))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(resolveConstantsBinding, resources.constants));
            return desc;
        }

    } // namespace

    bool validateSharcRadianceCacheConfig(const SharcRadianceCacheConfig& config) noexcept {
        return config.capacity >= 16 && isPowerOfTwo(config.capacity) && config.sparseTileSize != 0 &&
               config.accumulationFrameCount >= 1 && config.accumulationFrameCount <= 1024 &&
               config.responsiveAccumulationFrameCount >= 1 &&
               config.responsiveAccumulationFrameCount <= config.accumulationFrameCount &&
               config.responsiveFrameCount >= 1 && config.staleFrameCount >= 8 && config.staleFrameCount <= 1024 &&
               finite(config.sceneScale) && config.sceneScale > 0.0F && finite(config.logarithmBase) &&
               config.logarithmBase > 1.0F && finite(config.levelBias) && finite(config.radianceScale) &&
               config.radianceScale > 0.0F;
    }

    bool SharcHistoryPlan::isValid() const noexcept {
        return valid_;
    }

    SharcHistoryTracker::SharcHistoryTracker(SharcRadianceCacheConfig config) : config_(std::move(config)) {
        if (!validateSharcRadianceCacheConfig(config_)) {
            throw std::invalid_argument("SHARC history tracker requires a valid cache configuration.");
        }
    }

    SharcHistoryPlan SharcHistoryTracker::beginFrame(const glm::vec4& cameraPosition,
                                                     const SharcInvalidationInputs& invalidation) {
        if (!finite(cameraPosition)) {
            throw std::invalid_argument("SHARC camera position must be finite.");
        }
        if (pending_) {
            throw std::logic_error("SHARC history already has a pending frame.");
        }

        const bool fullReset =
            !initialized_ || invalidation.cameraCut || invalidation.topologyChanged || invalidation.geometryChanged;
        const bool responsiveChange =
            invalidation.materialChanged || invalidation.lightingChanged || invalidation.atmosphereChanged;

        SharcHistoryPlan plan;
        plan.valid_ = true;
        plan.resolveFrameIndex = static_cast<std::uint32_t>(submittedFrameCount_);
        plan.previousCameraPosition = fullReset ? cameraPosition : previousCameraPosition_;
        if (fullReset) {
            plan.invalidation = SharcInvalidationMode::FullReset;
            plan.accumulationFrameCount = config_.accumulationFrameCount;
            plan.responsiveFramesAfterSubmit = 0;
        } else if (responsiveChange || responsiveFramesRemaining_ != 0) {
            plan.invalidation = SharcInvalidationMode::ResponsiveDecay;
            plan.accumulationFrameCount = config_.responsiveAccumulationFrameCount;
            const std::uint32_t activeFrames =
                responsiveChange ? config_.responsiveFrameCount : responsiveFramesRemaining_;
            plan.responsiveFramesAfterSubmit = activeFrames > 0 ? activeFrames - 1 : 0;
        } else {
            plan.invalidation = SharcInvalidationMode::Preserve;
            plan.accumulationFrameCount = config_.accumulationFrameCount;
            plan.responsiveFramesAfterSubmit = 0;
        }

        pending_ = Pending{plan, cameraPosition};
        return plan;
    }

    void SharcHistoryTracker::commitSubmittedFrame() {
        if (!pending_) {
            return;
        }
        initialized_ = true;
        ++submittedFrameCount_;
        responsiveFramesRemaining_ = pending_->plan.responsiveFramesAfterSubmit;
        previousCameraPosition_ = pending_->currentCameraPosition;
        pending_.reset();
    }

    void SharcHistoryTracker::discardPendingFrame() noexcept {
        pending_.reset();
    }

    bool SharcHistoryTracker::initialized() const noexcept {
        return initialized_;
    }

    bool SharcHistoryTracker::hasPendingFrame() const noexcept {
        return pending_.has_value();
    }

    std::uint64_t SharcHistoryTracker::submittedFrameCount() const noexcept {
        return submittedFrameCount_;
    }

    std::uint32_t SharcHistoryTracker::responsiveFramesRemaining() const noexcept {
        return responsiveFramesRemaining_;
    }

    const glm::vec4& SharcHistoryTracker::previousCameraPosition() const noexcept {
        return previousCameraPosition_;
    }

    SharcGpuConstants buildSharcGpuConstants(const SharcRadianceCacheConfig& config, const SharcHistoryPlan& history,
                                             const SharcFrameParameters& frame) {
        if (!validateSharcRadianceCacheConfig(config) || !history.isValid() || !finite(frame.cameraPosition) ||
            !finite(frame.toSunWorld) || !finite(frame.sunRadiance) || frame.renderWidth == 0 ||
            frame.renderHeight == 0 || !finite(frame.minTraceDistance) || !finite(frame.maxTraceDistance) ||
            frame.minTraceDistance <= 0.0F || frame.maxTraceDistance <= frame.minTraceDistance) {
            throw std::invalid_argument("Cannot build SHARC GPU constants from invalid configuration or frame data.");
        }

        SharcGpuConstants result;
        result.cameraPositionSceneScale = frame.cameraPosition;
        result.cameraPositionSceneScale.w = config.sceneScale;
        result.previousCameraPositionLogarithmBase = history.previousCameraPosition;
        result.previousCameraPositionLogarithmBase.w = config.logarithmBase;
        result.toSunWorldRadianceScale = frame.toSunWorld;
        result.toSunWorldRadianceScale.w = config.radianceScale;
        result.sunRadiance = frame.sunRadiance;
        result.traceParameters = glm::vec4(frame.minTraceDistance, frame.maxTraceDistance, config.levelBias,
                                           config.enableAntiFireflyFilter ? 1.0F : 0.0F);
        result.cacheParameters = glm::uvec4(config.capacity, history.accumulationFrameCount, config.staleFrameCount,
                                            history.resolveFrameIndex);
        result.renderParameters = glm::uvec4(frame.renderWidth, frame.renderHeight, config.sparseTileSize, 0U);
        return result;
    }

    namespace detail {

        nvrhi::BufferDesc makeSharcBufferDesc(SharcBufferKind kind, std::uint32_t capacity, const char* debugName) {
            if (capacity == 0 || debugName == nullptr || debugName[0] == '\0') {
                throw std::invalid_argument("SHARC buffer description requires capacity and debug name.");
            }

            nvrhi::BufferDesc desc;
            desc.debugName = debugName;
            desc.keepInitialState = false;
            switch (kind) {
            case SharcBufferKind::HashEntries:
                desc.byteSize = byteSize(capacity, SharcBufferStrides::hashEntry);
                desc.structStride = SharcBufferStrides::hashEntry;
                desc.canHaveUAVs = true;
                desc.initialState = nvrhi::ResourceStates::Common;
                break;
            case SharcBufferKind::Accumulation:
                desc.byteSize = byteSize(capacity, SharcBufferStrides::accumulation);
                desc.structStride = SharcBufferStrides::accumulation;
                desc.canHaveUAVs = true;
                desc.initialState = nvrhi::ResourceStates::Common;
                break;
            case SharcBufferKind::Resolved:
                desc.byteSize = byteSize(capacity, SharcBufferStrides::resolved);
                desc.structStride = SharcBufferStrides::resolved;
                desc.canHaveUAVs = true;
                desc.initialState = nvrhi::ResourceStates::Common;
                break;
            case SharcBufferKind::Lock:
                desc.byteSize = byteSize(capacity, SharcBufferStrides::lock);
                desc.structStride = SharcBufferStrides::lock;
                desc.canHaveUAVs = true;
                desc.initialState = nvrhi::ResourceStates::Common;
                break;
            case SharcBufferKind::Statistics:
                desc.byteSize = SharcStatisticsLayout::byteSize;
                desc.structStride = sizeof(std::uint32_t);
                desc.canHaveUAVs = true;
                desc.initialState = nvrhi::ResourceStates::Common;
                break;
            case SharcBufferKind::StatisticsReadback:
                desc.byteSize = SharcStatisticsLayout::byteSize;
                desc.structStride = sizeof(std::uint32_t);
                desc.cpuAccess = nvrhi::CpuAccessMode::Read;
                desc.initialState = nvrhi::ResourceStates::CopyDest;
                break;
            case SharcBufferKind::Constants:
                desc.byteSize = sizeof(SharcGpuConstants);
                desc.isConstantBuffer = true;
                desc.cpuAccess = nvrhi::CpuAccessMode::Write;
                desc.initialState = nvrhi::ResourceStates::Common;
                break;
            }
            return desc;
        }

        nvrhi::BindingLayoutDesc makeSharcUpdateBindingLayoutDesc(std::uint32_t maxGeometryDescriptors,
                                                                  std::uint32_t maxMaterialTextureDescriptors) {
            if (maxGeometryDescriptors == 0 || maxGeometryDescriptors > std::numeric_limits<std::uint16_t>::max() ||
                maxMaterialTextureDescriptors == 0 ||
                maxMaterialTextureDescriptors > std::numeric_limits<std::uint16_t>::max()) {
                throw std::invalid_argument("SHARC descriptor capacities must fit NvRHI's uint16 array size.");
            }
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::AllRayTracing)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                       .setShaderResourceOffset(0)
                                       .setSamplerOffset(0)
                                       .setConstantBufferOffset(0)
                                       .setUnorderedAccessViewOffset(0))
                .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(updateTlasBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(updatePositionBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(updateNormalRoughnessBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(updateAlbedoMetallicBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(updateVertexBuffersBinding)
                             .setSize(maxGeometryDescriptors))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(updateIndexBuffersBinding)
                             .setSize(maxGeometryDescriptors))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(updateInstancesBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(updateMaterialsBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(updateHashEntriesBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(updateAccumulationBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(updateResolvedBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(updateLockBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(updateStatisticsBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(updateConstantsBinding))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(updateBaseColorTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(updateNormalRoughnessTexturesBinding)
                             .setSize(maxMaterialTextureDescriptors))
                .addItem(nvrhi::BindingLayoutItem::Sampler(updateMaterialSamplerBinding));
            return desc;
        }

        nvrhi::BindingLayoutDesc makeSharcResolveBindingLayoutDesc() {
            nvrhi::BindingLayoutDesc desc;
            desc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(0)
                .setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                       .setShaderResourceOffset(0)
                                       .setSamplerOffset(0)
                                       .setConstantBufferOffset(0)
                                       .setUnorderedAccessViewOffset(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(resolveHashEntriesBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(resolveAccumulationBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(resolveResolvedBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(resolveLockBinding))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(resolveStatisticsBinding))
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(resolveConstantsBinding));
            return desc;
        }

        SharcUpdateDispatchSize makeSharcUpdateDispatchSize(std::uint32_t width, std::uint32_t height,
                                                            std::uint32_t tileSize) {
            if (width == 0 || height == 0 || tileSize == 0) {
                throw std::invalid_argument("SHARC sparse update dispatch requires non-zero dimensions.");
            }
            return {width / tileSize + (width % tileSize == 0 ? 0U : 1U),
                    height / tileSize + (height % tileSize == 0 ? 0U : 1U)};
        }

        std::uint32_t makeSharcResolveDispatchSize(std::uint32_t capacity) {
            if (capacity == 0) {
                throw std::invalid_argument("SHARC resolve dispatch requires non-zero capacity.");
            }
            constexpr std::uint32_t threadGroupSize = 256;
            return capacity / threadGroupSize + (capacity % threadGroupSize == 0 ? 0U : 1U);
        }

    } // namespace detail

    struct SharcRadianceCache::Impl {
        struct PendingFrame {
            std::uint32_t frameSlot = 0;
            bool statisticsReadbackScheduled = false;
        };

        nvrhi::IDevice& device;
        SharcRadianceCacheConfig config;
        SharcHistoryTracker history;
        std::uint32_t maxGeometryDescriptors = 0;
        std::uint32_t maxMaterialTextureDescriptors = 0;
        std::vector<SharcUpdateFrameInputs> inputs;
        SharcNativeResources cacheResources;
        std::vector<nvrhi::BufferHandle> constants;
        std::vector<nvrhi::BufferHandle> statisticsReadbacks;
        std::vector<std::uint8_t> constantsInitialized;
        std::vector<std::uint8_t> statisticsReadbackReady;
        nvrhi::BindingLayoutHandle updateBindingLayout;
        nvrhi::BindingLayoutHandle resolveBindingLayout;
        nvrhi::rt::PipelineHandle updatePipeline;
        nvrhi::rt::ShaderTableHandle updateShaderTable;
        nvrhi::ComputePipelineHandle resolvePipeline;
        std::vector<nvrhi::BindingSetHandle> resolveBindingSets;
        std::optional<PendingFrame> pending;

        explicit Impl(const SharcRadianceCacheCreateInfo& createInfo)
            : device(*createInfo.device), config(createInfo.config), history(config),
              maxGeometryDescriptors(createInfo.maxGeometryDescriptors),
              maxMaterialTextureDescriptors(createInfo.maxMaterialTextureDescriptors),
              inputs(createInfo.frames.begin(), createInfo.frames.end()),
              constantsInitialized(createInfo.frameSlotCount, 0),
              statisticsReadbackReady(createInfo.frameSlotCount, 0) {
            const auto createBuffer = [&](detail::SharcBufferKind kind, const char* name) {
                nvrhi::BufferHandle result =
                    device.createBuffer(detail::makeSharcBufferDesc(kind, config.capacity, name));
                if (!result) {
                    throw std::runtime_error(std::string{"Failed to create SHARC buffer: "} + name);
                }
                return result;
            };
            cacheResources.hashEntries = createBuffer(detail::SharcBufferKind::HashEntries, "SHARC hash entries");
            cacheResources.accumulation = createBuffer(detail::SharcBufferKind::Accumulation, "SHARC accumulation");
            cacheResources.resolved = createBuffer(detail::SharcBufferKind::Resolved, "SHARC resolved");
            cacheResources.lock = createBuffer(detail::SharcBufferKind::Lock, "SHARC lock");
            cacheResources.statistics = createBuffer(detail::SharcBufferKind::Statistics, "SHARC statistics");

            updateBindingLayout = device.createBindingLayout(
                detail::makeSharcUpdateBindingLayoutDesc(maxGeometryDescriptors, maxMaterialTextureDescriptors));
            resolveBindingLayout = device.createBindingLayout(detail::makeSharcResolveBindingLayoutDesc());
            if (!updateBindingLayout || !resolveBindingLayout) {
                throw std::runtime_error("Failed to create SHARC binding layouts.");
            }

            ShaderLibrary shaders(device, createInfo.shaderDirectory);
            PipelineFactory pipelines(device);
            const nvrhi::ShaderHandle rayGeneration = shaders.loadRayTracingModule(
                "sharc_update.rgen.spv", nvrhi::ShaderType::RayGeneration, "sharcUpdateRayGenerationMain");
            const nvrhi::ShaderHandle radianceMiss = shaders.loadRayTracingModule(
                "sharc_update.radiance.rmiss.spv", nvrhi::ShaderType::Miss, "sharcUpdateRadianceMissMain");
            const nvrhi::ShaderHandle shadowMiss = shaders.loadRayTracingModule(
                "sharc_update.shadow.rmiss.spv", nvrhi::ShaderType::Miss, "sharcUpdateShadowMissMain");
            const nvrhi::ShaderHandle closestHit = shaders.loadRayTracingModule(
                "sharc_update.rchit.spv", nvrhi::ShaderType::ClosestHit, "sharcUpdateClosestHitMain");
            const std::array shaderExports = {
                RayTracingPipelineShaderDesc{"SharcUpdateRayGen", rayGeneration},
                RayTracingPipelineShaderDesc{"SharcUpdateRadianceMiss", radianceMiss},
                RayTracingPipelineShaderDesc{"SharcUpdateShadowMiss", shadowMiss},
            };
            const std::array hitGroups = {
                RayTracingHitGroupDesc{"SharcUpdateTriangleHit", closestHit, nullptr, nullptr, false},
            };
            const std::array updateLayouts = {updateBindingLayout, createInfo.atmosphereBindingLayout};
            RayTracingPipelineDesc updateDesc;
            updateDesc.shaders = shaderExports;
            updateDesc.hitGroups = hitGroups;
            updateDesc.globalBindingLayouts = updateLayouts;
            updateDesc.maxPayloadSize = 128;
            updateDesc.maxAttributeSize = sizeof(float) * 2;
            updateDesc.maxRecursionDepth = 2;
            updatePipeline = pipelines.createRayTracingPipeline(updateDesc);

            const std::array missEntries = {
                RayTracingShaderTableEntryDesc{"SharcUpdateRadianceMiss"},
                RayTracingShaderTableEntryDesc{"SharcUpdateShadowMiss"},
            };
            const std::array hitEntries = {RayTracingShaderTableEntryDesc{"SharcUpdateTriangleHit"}};
            RayTracingShaderTableDesc tableDesc;
            tableDesc.rayGenerationExport = "SharcUpdateRayGen";
            tableDesc.missShaders = missEntries;
            tableDesc.hitGroups = hitEntries;
            tableDesc.cached = true;
            tableDesc.maxEntries = 4;
            tableDesc.debugName = "SHARC sparse update shader table";
            updateShaderTable = pipelines.createRayTracingShaderTable(updatePipeline, tableDesc);

            const nvrhi::ShaderHandle resolveShader =
                shaders.loadComputeModule("sharc_resolve.comp.spv", "sharcResolveMain");
            const std::array resolveLayouts = {resolveBindingLayout};
            resolvePipeline = pipelines.createComputePipeline({resolveShader, resolveLayouts});

            constants.reserve(createInfo.frameSlotCount);
            statisticsReadbacks.reserve(createInfo.frameSlotCount);
            resolveBindingSets.reserve(createInfo.frameSlotCount);
            for (std::uint32_t frameSlot = 0; frameSlot < createInfo.frameSlotCount; ++frameSlot) {
                nvrhi::BufferHandle constant =
                    createBuffer(detail::SharcBufferKind::Constants, "SHARC frame constants");
                nvrhi::BufferHandle readback =
                    createBuffer(detail::SharcBufferKind::StatisticsReadback, "SHARC statistics readback");
                SharcNativeResources resources = cacheResources;
                resources.constants = constant;
                nvrhi::BindingSetHandle resolveBindingSet =
                    device.createBindingSet(makeResolveBindingSetDesc(resources), resolveBindingLayout);
                if (!resolveBindingSet) {
                    throw std::runtime_error("Failed to create SHARC resolve binding set.");
                }
                constants.push_back(std::move(constant));
                statisticsReadbacks.push_back(std::move(readback));
                resolveBindingSets.push_back(std::move(resolveBindingSet));
            }
        }
    };

    SharcRadianceCache::SharcRadianceCache(const SharcRadianceCacheCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.shaderDirectory.empty() || createInfo.frameSlotCount == 0 ||
            createInfo.maxGeometryDescriptors == 0 || createInfo.maxMaterialTextureDescriptors == 0 ||
            createInfo.frames.size() != createInfo.frameSlotCount || !createInfo.atmosphereBindingLayout ||
            !validateSharcRadianceCacheConfig(createInfo.config)) {
            throw std::invalid_argument(
                "SHARC cache requires device, shaders, matching frame slots, geometry capacity, and valid config.");
        }
        for (const SharcUpdateFrameInputs& inputs : createInfo.frames) {
            if (!complete(inputs)) {
                throw std::invalid_argument("SHARC cache received an incomplete G-buffer frame.");
            }
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    SharcRadianceCache::~SharcRadianceCache() = default;

    SharcGraphRecord SharcRadianceCache::record(FrameGraph& frameGraph, std::uint32_t frameSlot,
                                                bool frameSlotFenceWaited, const SharcFrameParameters& frameParameters,
                                                const SharcInvalidationInputs& invalidation,
                                                const RayTracingEnvironmentBindings& environment,
                                                const RayTracingEnvironmentGraphResources& environmentResources,
                                                const SharcUpdateFrameGraphInputs& inputs,
                                                const SharcUpdateSceneBindings& scene,
                                                const SharcUpdateSceneGraphResources& sceneResources) {
        if (!frameSlotFenceWaited) {
            throw std::logic_error("SHARC per-slot resources may only change after waiting for the slot fence.");
        }
        if (impl_->pending || impl_->history.hasPendingFrame()) {
            throw std::logic_error("SHARC cache already has a pending frame.");
        }
        if (frameSlot >= impl_->inputs.size() || frameSlot >= impl_->constants.size()) {
            throw std::out_of_range("SHARC frame slot is outside the configured range.");
        }
        if (!inputs.position.isValid() || !inputs.normalRoughness.isValid() || !inputs.albedoMetallic.isValid() ||
            !sceneResources.tlas.isValid() || !sceneResources.instances.isValid() ||
            !sceneResources.materials.isValid() || !complete(scene) || !environment.isValid() ||
            !environmentResources.isValid() || scene.geometry.size() > impl_->maxGeometryDescriptors ||
            sceneResources.vertices.size() != scene.geometry.size() ||
            sceneResources.indices.size() != scene.geometry.size() ||
            sceneResources.baseColorTextures.size() != scene.baseColorTextures.size() ||
            sceneResources.normalRoughnessTextures.size() != scene.normalRoughnessTextures.size()) {
            throw std::invalid_argument("SHARC record requires matching G-buffer and GPU Scene resources.");
        }
        const nvrhi::TextureDesc& positionDesc = impl_->inputs[frameSlot].position->getDesc();
        if (frameParameters.renderWidth != positionDesc.width || frameParameters.renderHeight != positionDesc.height) {
            throw std::invalid_argument("SHARC frame extent does not match the configured G-buffer.");
        }

        const SharcHistoryPlan history = impl_->history.beginFrame(frameParameters.cameraPosition, invalidation);
        try {
            const SharcGpuConstants constants = buildSharcGpuConstants(impl_->config, history, frameParameters);
            writeConstants(impl_->device, impl_->constants[frameSlot], constants);

            SharcNativeResources native = impl_->cacheResources;
            native.constants = impl_->constants[frameSlot];
            const nvrhi::BindingSetHandle updateBindingSet = impl_->device.createBindingSet(
                makeUpdateBindingSetDesc(impl_->inputs[frameSlot], scene, native, impl_->maxGeometryDescriptors,
                                         impl_->maxMaterialTextureDescriptors),
                impl_->updateBindingLayout);
            if (!updateBindingSet) {
                throw std::runtime_error("Failed to create SHARC update binding set.");
            }

            const nvrhi::ResourceStates cacheInitialState =
                impl_->history.initialized() ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::Common;
            const auto importCache = [&](std::string name, nvrhi::IBuffer* buffer, std::uint64_t size,
                                         nvrhi::ResourceStates initial, nvrhi::ResourceStates finalState) {
                return frameGraph.importBuffer(std::move(name), FrameGraphBufferDesc{.size = size,
                                                                                     .buffer = buffer,
                                                                                     .initialState = initial,
                                                                                     .finalState = finalState});
            };

            SharcFrameGraphResources graphResources;
            graphResources.hashEntries = importCache("sharc-hash-entries", native.hashEntries,
                                                     byteSize(impl_->config.capacity, SharcBufferStrides::hashEntry),
                                                     cacheInitialState, nvrhi::ResourceStates::UnorderedAccess);
            graphResources.accumulation =
                importCache("sharc-accumulation", native.accumulation,
                            byteSize(impl_->config.capacity, SharcBufferStrides::accumulation), cacheInitialState,
                            nvrhi::ResourceStates::UnorderedAccess);
            graphResources.resolved = importCache("sharc-resolved", native.resolved,
                                                  byteSize(impl_->config.capacity, SharcBufferStrides::resolved),
                                                  cacheInitialState, nvrhi::ResourceStates::UnorderedAccess);
            graphResources.lock =
                importCache("sharc-lock", native.lock, byteSize(impl_->config.capacity, SharcBufferStrides::lock),
                            cacheInitialState, nvrhi::ResourceStates::UnorderedAccess);
            graphResources.statistics = importCache(
                "sharc-statistics", native.statistics, SharcStatisticsLayout::byteSize,
                impl_->history.initialized() ? nvrhi::ResourceStates::CopySource : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::CopySource);
            graphResources.constants =
                importCache("sharc-constants-" + std::to_string(frameSlot), native.constants, sizeof(SharcGpuConstants),
                            impl_->constantsInitialized[frameSlot] != 0 ? nvrhi::ResourceStates::ConstantBuffer
                                                                        : nvrhi::ResourceStates::Common,
                            nvrhi::ResourceStates::ConstantBuffer);
            graphResources.statisticsReadback = importCache(
                "sharc-statistics-readback-" + std::to_string(frameSlot), impl_->statisticsReadbacks[frameSlot],
                SharcStatisticsLayout::byteSize, nvrhi::ResourceStates::CopyDest, nvrhi::ResourceStates::CopyDest);

            const bool fullReset = history.invalidation == SharcInvalidationMode::FullReset;
            const nvrhi::BufferHandle hashEntries = native.hashEntries;
            const nvrhi::BufferHandle accumulation = native.accumulation;
            const nvrhi::BufferHandle resolved = native.resolved;
            const nvrhi::BufferHandle lock = native.lock;
            const nvrhi::BufferHandle statistics = native.statistics;
            const FrameGraphPassHandle clearPass = frameGraph.addPass(
                "sharc-clear", FrameGraphPassType::Transfer,
                [graphResources, fullReset](FrameGraphBuilder& builder) {
                    builder.write(graphResources.statistics, nvrhi::ResourceStates::CopyDest);
                    if (fullReset) {
                        builder.write(graphResources.hashEntries, nvrhi::ResourceStates::CopyDest);
                        builder.write(graphResources.accumulation, nvrhi::ResourceStates::CopyDest);
                        builder.write(graphResources.resolved, nvrhi::ResourceStates::CopyDest);
                        builder.write(graphResources.lock, nvrhi::ResourceStates::CopyDest);
                    }
                },
                [hashEntries, accumulation, resolved, lock, statistics, fullReset](const FrameGraphContext& context) {
                    if (context.commandList == nullptr) {
                        throw std::logic_error("SHARC clear requires an NvRHI command list.");
                    }
                    context.commandList->clearBufferUInt(statistics, 0);
                    if (fullReset) {
                        context.commandList->clearBufferUInt(hashEntries, 0);
                        context.commandList->clearBufferUInt(accumulation, 0);
                        context.commandList->clearBufferUInt(resolved, 0);
                        context.commandList->clearBufferUInt(lock, 0);
                    }
                });

            const std::vector<FrameGraphResourceHandle> vertexResources(sceneResources.vertices.begin(),
                                                                        sceneResources.vertices.end());
            const std::vector<FrameGraphResourceHandle> indexResources(sceneResources.indices.begin(),
                                                                       sceneResources.indices.end());
            const std::vector<FrameGraphResourceHandle> baseColorResources(sceneResources.baseColorTextures.begin(),
                                                                           sceneResources.baseColorTextures.end());
            const std::vector<FrameGraphResourceHandle> normalRoughnessResources(
                sceneResources.normalRoughnessTextures.begin(), sceneResources.normalRoughnessTextures.end());
            const nvrhi::rt::ShaderTableHandle updateShaderTable = impl_->updateShaderTable;
            const detail::SharcUpdateDispatchSize updateDispatch = detail::makeSharcUpdateDispatchSize(
                frameParameters.renderWidth, frameParameters.renderHeight, impl_->config.sparseTileSize);
            const FrameGraphPassHandle updatePass = frameGraph.addPass(
                "sharc-sparse-update", FrameGraphPassType::RayTracing,
                [inputs, sceneResources, environmentResources, graphResources, clearPass, vertexResources,
                 indexResources, baseColorResources, normalRoughnessResources](FrameGraphBuilder& builder) {
                    builder.dependsOn(clearPass);
                    if (sceneResources.readyPass.isValid()) {
                        builder.dependsOn(sceneResources.readyPass);
                    }
                    builder.readAccelerationStructure(sceneResources.tlas);
                    builder.read(sceneResources.instances, nvrhi::ResourceStates::ShaderResource);
                    builder.read(sceneResources.materials, nvrhi::ResourceStates::ShaderResource);
                    for (const FrameGraphResourceHandle resource : vertexResources) {
                        builder.read(resource, nvrhi::ResourceStates::ShaderResource);
                    }
                    for (const FrameGraphResourceHandle resource : indexResources) {
                        builder.read(resource, nvrhi::ResourceStates::ShaderResource);
                    }
                    for (const FrameGraphResourceHandle resource : baseColorResources) {
                        builder.readTexture(resource, nvrhi::ResourceStates::ShaderResource);
                    }
                    for (const FrameGraphResourceHandle resource : normalRoughnessResources) {
                        builder.readTexture(resource, nvrhi::ResourceStates::ShaderResource);
                    }
                    builder.readTexture(inputs.position);
                    builder.readTexture(inputs.normalRoughness);
                    builder.readTexture(inputs.albedoMetallic);
                    for (const FrameGraphResourceHandle lut : environmentResources.atmosphere.textures) {
                        builder.readTexture(lut);
                    }
                    builder.read(environmentResources.atmosphere.constants, nvrhi::ResourceStates::ConstantBuffer);
                    builder.read(graphResources.constants, nvrhi::ResourceStates::ConstantBuffer);
                    builder.readWrite(graphResources.hashEntries, nvrhi::ResourceStates::UnorderedAccess);
                    builder.readWrite(graphResources.accumulation, nvrhi::ResourceStates::UnorderedAccess);
                    builder.read(graphResources.resolved, nvrhi::ResourceStates::UnorderedAccess);
                    builder.readWrite(graphResources.lock, nvrhi::ResourceStates::UnorderedAccess);
                    builder.readWrite(graphResources.statistics, nvrhi::ResourceStates::UnorderedAccess);
                },
                [atmosphereBindingSet = environment.atmosphere, updateBindingSet, updateShaderTable,
                 updateDispatch](const FrameGraphContext& context) {
                    if (context.commandList == nullptr) {
                        throw std::logic_error("SHARC sparse update requires an NvRHI command list.");
                    }
                    nvrhi::rt::State state;
                    state.setShaderTable(updateShaderTable)
                        .addBindingSet(updateBindingSet)
                        .addBindingSet(atmosphereBindingSet);
                    detail::recordSharcUpdateDispatch(*context.commandList, state, updateDispatch);
                });

            const nvrhi::ComputePipelineHandle resolvePipeline = impl_->resolvePipeline;
            const nvrhi::BindingSetHandle resolveBindingSet = impl_->resolveBindingSets[frameSlot];
            const std::uint32_t resolveDispatch = detail::makeSharcResolveDispatchSize(impl_->config.capacity);
            const FrameGraphPassHandle resolvePass = frameGraph.addPass(
                "sharc-resolve-evict", FrameGraphPassType::Compute,
                [graphResources, updatePass](FrameGraphBuilder& builder) {
                    builder.dependsOn(updatePass);
                    builder.read(graphResources.constants, nvrhi::ResourceStates::ConstantBuffer);
                    builder.readWrite(graphResources.hashEntries, nvrhi::ResourceStates::UnorderedAccess);
                    builder.readWrite(graphResources.accumulation, nvrhi::ResourceStates::UnorderedAccess);
                    builder.readWrite(graphResources.resolved, nvrhi::ResourceStates::UnorderedAccess);
                    builder.read(graphResources.lock, nvrhi::ResourceStates::UnorderedAccess);
                    builder.readWrite(graphResources.statistics, nvrhi::ResourceStates::UnorderedAccess);
                },
                [resolvePipeline, resolveBindingSet, resolveDispatch](const FrameGraphContext& context) {
                    if (context.commandList == nullptr) {
                        throw std::logic_error("SHARC resolve requires an NvRHI command list.");
                    }
                    nvrhi::ComputeState state;
                    state.setPipeline(resolvePipeline).addBindingSet(resolveBindingSet);
                    detail::recordSharcResolveDispatch(*context.commandList, state, resolveDispatch);
                });

            impl_->pending = Impl::PendingFrame{frameSlot, false};
            return SharcGraphRecord{.native = std::move(native),
                                    .resources = graphResources,
                                    .clearPass = clearPass,
                                    .updatePass = updatePass,
                                    .resolvePass = resolvePass,
                                    .invalidation = history.invalidation};
        } catch (...) {
            impl_->history.discardPendingFrame();
            throw;
        }
    }

    FrameGraphPassHandle SharcRadianceCache::recordStatisticsReadback(FrameGraph& frameGraph,
                                                                      const SharcGraphRecord& record,
                                                                      FrameGraphPassHandle queryPass) {
        if (!impl_->pending || !record.isValid() || !queryPass.isValid()) {
            throw std::invalid_argument("SHARC statistics readback requires a pending cache record and query pass.");
        }
        if (impl_->pending->statisticsReadbackScheduled) {
            throw std::logic_error("SHARC statistics readback is already scheduled for this frame.");
        }
        if (record.native.hashEntries.Get() != impl_->cacheResources.hashEntries.Get() ||
            record.native.statistics.Get() != impl_->cacheResources.statistics.Get()) {
            throw std::invalid_argument("SHARC statistics readback received resources from another cache owner.");
        }

        const nvrhi::BufferHandle statistics = impl_->cacheResources.statistics;
        const nvrhi::BufferHandle readback = impl_->statisticsReadbacks[impl_->pending->frameSlot];
        const FrameGraphPassHandle pass = frameGraph.addPass(
            "sharc-statistics-readback", FrameGraphPassType::Transfer,
            [resources = record.resources, queryPass](FrameGraphBuilder& builder) {
                builder.dependsOn(queryPass);
                builder.read(resources.statistics, nvrhi::ResourceStates::CopySource);
                builder.write(resources.statisticsReadback, nvrhi::ResourceStates::CopyDest);
            },
            [statistics, readback](const FrameGraphContext& context) {
                if (context.commandList == nullptr) {
                    throw std::logic_error("SHARC statistics readback requires an NvRHI command list.");
                }
                context.commandList->copyBuffer(readback, 0, statistics, 0, SharcStatisticsLayout::byteSize);
            });
        impl_->pending->statisticsReadbackScheduled = true;
        return pass;
    }

    void SharcRadianceCache::commitSubmittedFrame() {
        if (!impl_->pending) {
            return;
        }
        const Impl::PendingFrame pending = *impl_->pending;
        impl_->history.commitSubmittedFrame();
        impl_->constantsInitialized[pending.frameSlot] = 1;
        impl_->statisticsReadbackReady[pending.frameSlot] = pending.statisticsReadbackScheduled ? 1 : 0;
        impl_->pending.reset();
    }

    void SharcRadianceCache::discardPendingFrame() noexcept {
        impl_->history.discardPendingFrame();
        impl_->pending.reset();
    }

    bool SharcRadianceCache::hasPendingFrame() const noexcept {
        return impl_->pending.has_value();
    }

    std::optional<SharcStatistics> SharcRadianceCache::readbackStatistics(std::uint32_t frameSlot,
                                                                          bool frameSlotFenceWaited) {
        if (!frameSlotFenceWaited) {
            throw std::logic_error("SHARC statistics may only be mapped after waiting for the slot fence.");
        }
        if (frameSlot >= impl_->statisticsReadbacks.size()) {
            throw std::out_of_range("SHARC statistics frame slot is outside the configured range.");
        }
        if (impl_->statisticsReadbackReady[frameSlot] == 0) {
            return std::nullopt;
        }
        void* mapped = impl_->device.mapBuffer(impl_->statisticsReadbacks[frameSlot], nvrhi::CpuAccessMode::Read);
        if (mapped == nullptr) {
            throw std::runtime_error("Failed to map SHARC statistics readback buffer.");
        }
        std::array<std::uint32_t, SharcStatisticsLayout::count> counters{};
        std::memcpy(counters.data(), mapped, SharcStatisticsLayout::byteSize);
        impl_->device.unmapBuffer(impl_->statisticsReadbacks[frameSlot]);
        return SharcStatistics{.queryHitCount = counters[SharcStatisticsLayout::queryHit],
                               .updateCount = counters[SharcStatisticsLayout::update],
                               .overflowCount = counters[SharcStatisticsLayout::overflow],
                               .occupancyCount = counters[SharcStatisticsLayout::occupancy]};
    }

    const SharcRadianceCacheConfig& SharcRadianceCache::config() const noexcept {
        return impl_->config;
    }

    const SharcHistoryTracker& SharcRadianceCache::history() const noexcept {
        return impl_->history;
    }

} // namespace lumin::render::gi

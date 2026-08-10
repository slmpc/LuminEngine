#include "render/gi/NrdDenoiser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lumin::render::gi {
    namespace {

        constexpr nrd::Identifier reblurIdentifier = 1;

        [[nodiscard]] bool validHistoryAction(core::HistoryAction action) noexcept {
            return static_cast<std::uint8_t>(action) <= static_cast<std::uint8_t>(core::HistoryAction::FullReset);
        }

        [[nodiscard]] std::size_t nrdHistoryIndex(core::HistoryDomain domain) {
            if (domain == core::HistoryDomain::NrdDiffuse) {
                return 0;
            }
            if (domain == core::HistoryDomain::NrdSpecular) {
                return 1;
            }
            throw std::invalid_argument("NRD history tracker only owns diffuse and specular domains.");
        }

        [[nodiscard]] bool needsRestart(core::HistoryAction action) noexcept {
            return action != core::HistoryAction::Keep;
        }

        [[nodiscard]] nvrhi::VulkanBindingOffsets bindingOffsets(const nrd::LibraryDesc& libraryDesc) {
            return nvrhi::VulkanBindingOffsets()
                .setSamplerOffset(libraryDesc.spirvBindingOffsets.samplerOffset)
                .setShaderResourceOffset(libraryDesc.spirvBindingOffsets.textureOffset)
                .setConstantBufferOffset(libraryDesc.spirvBindingOffsets.constantBufferOffset)
                .setUnorderedAccessViewOffset(libraryDesc.spirvBindingOffsets.storageTextureAndBufferOffset);
        }

        [[nodiscard]] bool finiteMatrix(const std::array<float, 16>& matrix) noexcept {
            return std::ranges::all_of(matrix, [](float value) {
                return std::isfinite(value);
            });
        }

        [[nodiscard]] bool validJitter(const std::array<float, 2>& jitter) noexcept {
            return std::ranges::all_of(jitter, [](float value) {
                return std::isfinite(value) && value >= -0.5F && value <= 0.5F;
            });
        }

        void requireNrdSuccess(nrd::Result result, std::string_view operation) {
            if (result != nrd::Result::SUCCESS) {
                throw std::runtime_error(std::string(operation) + " failed with NRD result " +
                                         std::to_string(static_cast<std::uint32_t>(result)) + ".");
            }
        }

        [[nodiscard]] std::uint32_t divideRoundUp(std::uint32_t value, std::uint32_t divisor) {
            if (divisor == 0) {
                throw std::invalid_argument("NRD texture downsample factor cannot be zero.");
            }
            return value / divisor + (value % divisor != 0 ? 1U : 0U);
        }

        [[nodiscard]] bool complete(const NrdTextureBinding& binding) noexcept {
            return binding.texture != nullptr && binding.graphResource.isValid();
        }

        void validateTextureShape(const NrdTextureBinding& binding, core::RenderExtent extent, std::string_view role) {
            if (!complete(binding)) {
                throw std::invalid_argument(std::string(role) + " requires matching physical and graph resources.");
            }
            const nvrhi::TextureDesc& desc = binding.texture->getDesc();
            if (desc.dimension != nvrhi::TextureDimension::Texture2D || desc.width != extent.width ||
                desc.height != extent.height || desc.depth != 1 || desc.arraySize != 1 || desc.mipLevels != 1 ||
                desc.sampleCount != 1 || !desc.isShaderResource || !desc.isUAV) {
                throw std::invalid_argument(std::string(role) +
                                            " must be a full-resolution, single-mip 2D SRV/UAV texture.");
            }
        }

        [[nodiscard]] bool rgbaFloat16OrBetter(nvrhi::Format format) noexcept {
            return format == nvrhi::Format::RGBA16_FLOAT || format == nvrhi::Format::RGBA32_FLOAT;
        }

        [[nodiscard]] bool normalFormatCompatible(nrd::NormalEncoding encoding, nvrhi::Format format) noexcept {
            switch (encoding) {
            case nrd::NormalEncoding::RGBA8_UNORM:
                return format == nvrhi::Format::RGBA8_UNORM;
            case nrd::NormalEncoding::RGBA8_SNORM:
                return format == nvrhi::Format::RGBA8_SNORM;
            case nrd::NormalEncoding::R10_G10_B10_A2_UNORM:
                return format == nvrhi::Format::R10G10B10A2_UNORM;
            case nrd::NormalEncoding::RGBA16_UNORM:
                return format == nvrhi::Format::RGBA16_UNORM;
            case nrd::NormalEncoding::RGBA16_SNORM:
                return format == nvrhi::Format::RGBA16_SNORM || format == nvrhi::Format::RGBA16_FLOAT ||
                       format == nvrhi::Format::RGBA32_FLOAT;
            default:
                return false;
            }
        }

        void validateSignals(const NrdSignalBindings& signals, core::RenderExtent extent,
                             const nrd::LibraryDesc& libraryDesc) {
            validateTextureShape(signals.diffuseRadianceHitDistance, extent, "NRD diffuse input");
            validateTextureShape(signals.specularRadianceHitDistance, extent, "NRD specular input");
            validateTextureShape(signals.viewZ, extent, "NRD viewZ input");
            validateTextureShape(signals.normalRoughness, extent, "NRD normal/roughness input");
            validateTextureShape(signals.motion, extent, "NRD motion input");

            if (!rgbaFloat16OrBetter(signals.diffuseRadianceHitDistance.texture->getDesc().format) ||
                !rgbaFloat16OrBetter(signals.specularRadianceHitDistance.texture->getDesc().format)) {
                throw std::invalid_argument("REBLUR radiance/hit-distance inputs require RGBA16_FLOAT or better.");
            }
            const nvrhi::Format viewZFormat = signals.viewZ.texture->getDesc().format;
            if (viewZFormat != nvrhi::Format::R16_FLOAT && viewZFormat != nvrhi::Format::R32_FLOAT) {
                throw std::invalid_argument("NRD viewZ requires R16_FLOAT or R32_FLOAT.");
            }
            const nvrhi::Format motionFormat = signals.motion.texture->getDesc().format;
            if (motionFormat != nvrhi::Format::RG16_FLOAT && motionFormat != nvrhi::Format::RG32_FLOAT) {
                throw std::invalid_argument("NRD screen-space motion requires RG16_FLOAT or RG32_FLOAT.");
            }
            if (!normalFormatCompatible(libraryDesc.normalEncoding,
                                        signals.normalRoughness.texture->getDesc().format)) {
                throw std::invalid_argument("NRD normal/roughness texture does not match LibraryDesc::normalEncoding.");
            }
        }

        [[nodiscard]] nvrhi::TextureDesc makeOutputTextureDesc(core::RenderExtent extent, const char* name) {
            nvrhi::TextureDesc desc;
            desc.width = extent.width;
            desc.height = extent.height;
            desc.format = nvrhi::Format::RGBA16_FLOAT;
            desc.dimension = nvrhi::TextureDimension::Texture2D;
            desc.debugName = name;
            desc.isShaderResource = true;
            desc.isUAV = true;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            return desc;
        }

        [[nodiscard]] nvrhi::TextureHandle createTexture(nvrhi::IDevice& device, const nvrhi::TextureDesc& desc) {
            nvrhi::TextureHandle texture = device.createTexture(desc);
            if (!texture) {
                throw std::runtime_error("Failed to create NRD texture: " + desc.debugName);
            }
            return texture;
        }

        [[nodiscard]] nvrhi::BindingLayoutHandle
        createBindingLayout(nvrhi::IDevice& device, const nvrhi::BindingLayoutDesc& desc, std::string_view role) {
            nvrhi::BindingLayoutHandle layout = device.createBindingLayout(desc);
            if (!layout) {
                throw std::runtime_error("Failed to create NRD " + std::string(role) + " binding layout.");
            }
            return layout;
        }

        [[nodiscard]] FrameGraphTextureDesc importedTextureDesc(nvrhi::ITexture* texture, bool initialized) {
            FrameGraphTextureDesc desc;
            desc.texture = texture;
            desc.initialState = initialized ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
            desc.finalState = nvrhi::ResourceStates::ShaderResource;
            return desc;
        }

    } // namespace

    NrdHistoryPlan NrdHistoryTracker::beginFrame(core::RenderSequence sequence, core::HistoryAction diffuseAction,
                                                 core::HistoryAction specularAction, bool cameraCut,
                                                 bool resourcesRecreated) {
        if (activePlan_) {
            throw std::logic_error("NRD history already has an active frame.");
        }
        if (!sequence.isValid() || !validHistoryAction(diffuseAction) || !validHistoryAction(specularAction)) {
            throw std::invalid_argument("NRD history requires a valid sequence and history actions.");
        }
        if (states_[0].lastSuccessfulSequence.isValid() &&
            sequence.value() <= states_[0].lastSuccessfulSequence.value()) {
            throw std::invalid_argument("NRD render sequence must advance after a successful submit.");
        }

        NrdHistoryPlan plan;
        plan.sequence = sequence;
        plan.frameIndex = static_cast<std::uint32_t>(committedFrameCount_);
        plan.requestedDiffuseAction = diffuseAction;
        plan.requestedSpecularAction = specularAction;

        const bool firstUse = !states_[0].valid || !states_[1].valid;
        const bool restart =
            firstUse || cameraCut || resourcesRecreated || needsRestart(diffuseAction) || needsRestart(specularAction);
        if (firstUse || resourcesRecreated) {
            plan.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
        } else if (restart) {
            plan.accumulationMode = nrd::AccumulationMode::RESTART;
        }
        if (restart) {
            plan.effectiveDiffuseAction = core::HistoryAction::FullReset;
            plan.effectiveSpecularAction = core::HistoryAction::FullReset;
        }

        activePlan_ = std::make_unique<NrdHistoryPlan>(plan);
        return plan;
    }

    void NrdHistoryTracker::commitSubmittedFrame(core::RenderSequence sequence) {
        requireActive(sequence);
        const std::array actions = {activePlan_->effectiveDiffuseAction, activePlan_->effectiveSpecularAction};
        for (std::size_t index = 0; index < states_.size(); ++index) {
            NrdHistoryState& state = states_[index];
            state.valid = true;
            state.acceptedFrameCount++;
            state.lastCommittedAction = actions[index];
            state.lastSuccessfulSequence = sequence;
            if (actions[index] == core::HistoryAction::FullReset) {
                state.resetEpoch++;
            }
        }
        committedFrameCount_++;
        activePlan_.reset();
    }

    void NrdHistoryTracker::discardFrame(core::RenderSequence sequence) {
        requireActive(sequence);
        activePlan_.reset();
    }

    bool NrdHistoryTracker::hasActiveFrame() const noexcept {
        return activePlan_ != nullptr;
    }

    const NrdHistoryState& NrdHistoryTracker::state(core::HistoryDomain domain) const {
        return states_[nrdHistoryIndex(domain)];
    }

    void NrdHistoryTracker::requireActive(core::RenderSequence sequence) const {
        if (!activePlan_ || activePlan_->sequence != sequence) {
            throw std::logic_error("NRD history completion does not match the active frame.");
        }
    }

    bool NrdPreparedFrame::isValid() const noexcept {
        return token_ != 0;
    }

    core::RenderSequence NrdPreparedFrame::sequence() const noexcept {
        return sequence_;
    }

    core::FrameSlotIndex NrdPreparedFrame::frameSlot() const noexcept {
        return frameSlot_;
    }

    const NrdHistoryPlan& NrdPreparedFrame::historyPlan() const {
        if (!isValid()) {
            throw std::logic_error("NRD prepared frame is invalid.");
        }
        return historyPlan_;
    }

    const NrdGraphOutputs& NrdPreparedFrame::outputs() const noexcept {
        return outputs_;
    }

    std::span<const FrameGraphPassHandle> NrdPreparedFrame::passes() const noexcept {
        return passes_;
    }

    namespace detail {

        nvrhi::Format nrdFormatToNvrhi(nrd::Format format) {
            switch (format) {
            case nrd::Format::R8_UNORM:
                return nvrhi::Format::R8_UNORM;
            case nrd::Format::R8_SNORM:
                return nvrhi::Format::R8_SNORM;
            case nrd::Format::R8_UINT:
                return nvrhi::Format::R8_UINT;
            case nrd::Format::R8_SINT:
                return nvrhi::Format::R8_SINT;
            case nrd::Format::RG8_UNORM:
                return nvrhi::Format::RG8_UNORM;
            case nrd::Format::RG8_SNORM:
                return nvrhi::Format::RG8_SNORM;
            case nrd::Format::RG8_UINT:
                return nvrhi::Format::RG8_UINT;
            case nrd::Format::RG8_SINT:
                return nvrhi::Format::RG8_SINT;
            case nrd::Format::RGBA8_UNORM:
                return nvrhi::Format::RGBA8_UNORM;
            case nrd::Format::RGBA8_SNORM:
                return nvrhi::Format::RGBA8_SNORM;
            case nrd::Format::RGBA8_UINT:
                return nvrhi::Format::RGBA8_UINT;
            case nrd::Format::RGBA8_SINT:
                return nvrhi::Format::RGBA8_SINT;
            case nrd::Format::RGBA8_SRGB:
                return nvrhi::Format::SRGBA8_UNORM;
            case nrd::Format::R16_UNORM:
                return nvrhi::Format::R16_UNORM;
            case nrd::Format::R16_SNORM:
                return nvrhi::Format::R16_SNORM;
            case nrd::Format::R16_UINT:
                return nvrhi::Format::R16_UINT;
            case nrd::Format::R16_SINT:
                return nvrhi::Format::R16_SINT;
            case nrd::Format::R16_SFLOAT:
                return nvrhi::Format::R16_FLOAT;
            case nrd::Format::RG16_UNORM:
                return nvrhi::Format::RG16_UNORM;
            case nrd::Format::RG16_SNORM:
                return nvrhi::Format::RG16_SNORM;
            case nrd::Format::RG16_UINT:
                return nvrhi::Format::RG16_UINT;
            case nrd::Format::RG16_SINT:
                return nvrhi::Format::RG16_SINT;
            case nrd::Format::RG16_SFLOAT:
                return nvrhi::Format::RG16_FLOAT;
            case nrd::Format::RGBA16_UNORM:
                return nvrhi::Format::RGBA16_UNORM;
            case nrd::Format::RGBA16_SNORM:
                return nvrhi::Format::RGBA16_SNORM;
            case nrd::Format::RGBA16_UINT:
                return nvrhi::Format::RGBA16_UINT;
            case nrd::Format::RGBA16_SINT:
                return nvrhi::Format::RGBA16_SINT;
            case nrd::Format::RGBA16_SFLOAT:
                return nvrhi::Format::RGBA16_FLOAT;
            case nrd::Format::R32_UINT:
                return nvrhi::Format::R32_UINT;
            case nrd::Format::R32_SINT:
                return nvrhi::Format::R32_SINT;
            case nrd::Format::R32_SFLOAT:
                return nvrhi::Format::R32_FLOAT;
            case nrd::Format::RG32_UINT:
                return nvrhi::Format::RG32_UINT;
            case nrd::Format::RG32_SINT:
                return nvrhi::Format::RG32_SINT;
            case nrd::Format::RG32_SFLOAT:
                return nvrhi::Format::RG32_FLOAT;
            case nrd::Format::RGB32_UINT:
                return nvrhi::Format::RGB32_UINT;
            case nrd::Format::RGB32_SINT:
                return nvrhi::Format::RGB32_SINT;
            case nrd::Format::RGB32_SFLOAT:
                return nvrhi::Format::RGB32_FLOAT;
            case nrd::Format::RGBA32_UINT:
                return nvrhi::Format::RGBA32_UINT;
            case nrd::Format::RGBA32_SINT:
                return nvrhi::Format::RGBA32_SINT;
            case nrd::Format::RGBA32_SFLOAT:
                return nvrhi::Format::RGBA32_FLOAT;
            case nrd::Format::R10_G10_B10_A2_UNORM:
                return nvrhi::Format::R10G10B10A2_UNORM;
            case nrd::Format::R11_G11_B10_UFLOAT:
                return nvrhi::Format::R11G11B10_FLOAT;
            default:
                throw std::invalid_argument("NRD format has no NvRHI equivalent.");
            }
        }

        nvrhi::Format nrdNormalEncodingFormat(nrd::NormalEncoding encoding) {
            switch (encoding) {
            case nrd::NormalEncoding::RGBA8_UNORM:
                return nvrhi::Format::RGBA8_UNORM;
            case nrd::NormalEncoding::RGBA8_SNORM:
                return nvrhi::Format::RGBA8_SNORM;
            case nrd::NormalEncoding::R10_G10_B10_A2_UNORM:
                return nvrhi::Format::R10G10B10A2_UNORM;
            case nrd::NormalEncoding::RGBA16_UNORM:
                return nvrhi::Format::RGBA16_UNORM;
            case nrd::NormalEncoding::RGBA16_SNORM:
                return nvrhi::Format::RGBA16_SNORM;
            default:
                throw std::invalid_argument("NRD normal encoding is invalid.");
            }
        }

        NrdPoolTexturePlan makeNrdPoolTexturePlan(NrdPoolKind pool, std::uint16_t index,
                                                  const nrd::TextureDesc& texture, core::RenderExtent extent,
                                                  const char* debugName) {
            if (extent.isEmpty() || debugName == nullptr || debugName[0] == '\0') {
                throw std::invalid_argument("NRD pool texture requires extent and debug name.");
            }
            NrdPoolTexturePlan plan;
            plan.pool = pool;
            plan.index = index;
            plan.texture.width = divideRoundUp(extent.width, texture.downsampleFactor);
            plan.texture.height = divideRoundUp(extent.height, texture.downsampleFactor);
            plan.texture.format = nrdFormatToNvrhi(texture.format);
            plan.texture.dimension = nvrhi::TextureDimension::Texture2D;
            plan.texture.debugName = debugName;
            plan.texture.isShaderResource = true;
            plan.texture.isUAV = true;
            plan.texture.initialState = nvrhi::ResourceStates::Common;
            plan.texture.keepInitialState = false;
            return plan;
        }

        NrdResourceReference translateNrdResource(const nrd::ResourceDesc& resource,
                                                  const nrd::InstanceDesc& instanceDesc) {
            if (resource.descriptorType != nrd::DescriptorType::TEXTURE &&
                resource.descriptorType != nrd::DescriptorType::STORAGE_TEXTURE) {
                throw std::invalid_argument("NRD resource descriptor type is invalid.");
            }
            NrdResourceReference result;
            if (resource.type == nrd::ResourceType::PERMANENT_POOL) {
                if (resource.indexInPool >= instanceDesc.permanentPoolSize) {
                    throw std::out_of_range("NRD permanent pool index is outside InstanceDesc.");
                }
                result.source = NrdResourceSource::PermanentPool;
                result.poolIndex = resource.indexInPool;
            } else if (resource.type == nrd::ResourceType::TRANSIENT_POOL) {
                if (resource.indexInPool >= instanceDesc.transientPoolSize) {
                    throw std::out_of_range("NRD transient pool index is outside InstanceDesc.");
                }
                result.source = NrdResourceSource::TransientPool;
                result.poolIndex = resource.indexInPool;
            } else if (static_cast<std::uint32_t>(resource.type) <
                       static_cast<std::uint32_t>(nrd::ResourceType::TRANSIENT_POOL)) {
                result.source = NrdResourceSource::User;
                result.userType = resource.type;
            } else {
                throw std::invalid_argument("NRD ResourceType is invalid.");
            }
            return result;
        }

        NrdPipelineBindingLayouts makeNrdPipelineBindingLayouts(const nrd::LibraryDesc& libraryDesc,
                                                                const nrd::InstanceDesc& instanceDesc,
                                                                const nrd::PipelineDesc& pipelineDesc) {
            if (pipelineDesc.resourceRangesNum > 2 ||
                (pipelineDesc.resourceRangesNum != 0 && pipelineDesc.resourceRanges == nullptr)) {
                throw std::invalid_argument("NRD pipeline contains invalid resource ranges.");
            }

            NrdPipelineBindingLayouts layouts;
            const nvrhi::VulkanBindingOffsets offsets = bindingOffsets(libraryDesc);
            layouts.resources.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(instanceDesc.resourcesSpaceIndex)
                .setBindingOffsets(offsets);
            for (std::uint32_t rangeIndex = 0; rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex) {
                const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[rangeIndex];
                for (std::uint32_t descriptorIndex = 0; descriptorIndex < range.descriptorsNum; ++descriptorIndex) {
                    const std::uint32_t slot = instanceDesc.resourcesBaseRegisterIndex + descriptorIndex;
                    if (range.descriptorType == nrd::DescriptorType::TEXTURE) {
                        layouts.resources.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
                    } else if (range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE) {
                        layouts.resources.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
                    } else {
                        throw std::invalid_argument("NRD pipeline resource range type is invalid.");
                    }
                }
            }

            layouts.constantsAndSamplers.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(instanceDesc.constantBufferAndSamplersSpaceIndex)
                .setBindingOffsets(offsets)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(instanceDesc.constantBufferRegisterIndex));
            for (std::uint32_t index = 0; index < instanceDesc.samplersNum; ++index) {
                layouts.constantsAndSamplers.addItem(
                    nvrhi::BindingLayoutItem::Sampler(instanceDesc.samplersBaseRegisterIndex + index));
            }
            return layouts;
        }

        std::vector<NrdDispatchPlan> makeNrdDispatchPlans(const nrd::InstanceDesc& instanceDesc,
                                                          std::span<const nrd::DispatchDesc> dispatches) {
            std::vector<NrdDispatchPlan> plans;
            plans.reserve(dispatches.size());
            for (const nrd::DispatchDesc& dispatch : dispatches) {
                if (dispatch.pipelineIndex >= instanceDesc.pipelinesNum || dispatch.gridWidth == 0 ||
                    dispatch.gridHeight == 0 || (dispatch.resourcesNum != 0 && dispatch.resources == nullptr) ||
                    (dispatch.constantBufferDataSize != 0 && dispatch.constantBufferData == nullptr) ||
                    dispatch.constantBufferDataSize > instanceDesc.constantBufferMaxDataSize) {
                    throw std::invalid_argument("NRD dispatch descriptor is invalid.");
                }
                const nrd::PipelineDesc& pipeline = instanceDesc.pipelines[dispatch.pipelineIndex];
                NrdDispatchPlan plan;
                plan.name = dispatch.name != nullptr && dispatch.name[0] != '\0' ? dispatch.name : "unnamed";
                plan.pipelineIndex = dispatch.pipelineIndex;
                plan.gridWidth = dispatch.gridWidth;
                plan.gridHeight = dispatch.gridHeight;
                plan.constantDataMatchesPreviousDispatch = dispatch.constantBufferDataMatchesPreviousDispatch;
                if (dispatch.constantBufferDataSize != 0) {
                    plan.constantData.assign(dispatch.constantBufferData,
                                             dispatch.constantBufferData + dispatch.constantBufferDataSize);
                }

                std::uint32_t resourceIndex = 0;
                for (std::uint32_t rangeIndex = 0; rangeIndex < pipeline.resourceRangesNum; ++rangeIndex) {
                    const nrd::ResourceRangeDesc& range = pipeline.resourceRanges[rangeIndex];
                    for (std::uint32_t descriptorIndex = 0; descriptorIndex < range.descriptorsNum; ++descriptorIndex) {
                        if (resourceIndex >= dispatch.resourcesNum) {
                            throw std::invalid_argument("NRD dispatch has fewer resources than its pipeline ranges.");
                        }
                        const nrd::ResourceDesc& resource = dispatch.resources[resourceIndex++];
                        if (resource.descriptorType != range.descriptorType) {
                            throw std::invalid_argument("NRD dispatch resource order disagrees with pipeline ranges.");
                        }
                        plan.resources.push_back(NrdDispatchResourcePlan{
                            resource.descriptorType,
                            instanceDesc.resourcesBaseRegisterIndex + descriptorIndex,
                            translateNrdResource(resource, instanceDesc),
                        });
                    }
                }
                if (resourceIndex != dispatch.resourcesNum) {
                    throw std::invalid_argument("NRD dispatch has more resources than its pipeline ranges.");
                }
                plans.push_back(std::move(plan));
            }
            return plans;
        }

        std::vector<std::vector<std::uint8_t>> makeNrdConstantUploads(std::span<const NrdDispatchPlan> dispatches) {
            constexpr std::size_t minimumVolatileUploadSize = 16;
            std::vector<std::vector<std::uint8_t>> uploads(dispatches.size());
            bool hasCommandListVersion = false;
            for (std::size_t index = 0; index < dispatches.size(); ++index) {
                const NrdDispatchPlan& dispatch = dispatches[index];
                if (!hasCommandListVersion) {
                    uploads[index] = dispatch.constantData;
                    if (uploads[index].empty()) {
                        uploads[index].resize(minimumVolatileUploadSize, 0);
                    }
                    hasCommandListVersion = true;
                    continue;
                }
                if (!dispatch.constantDataMatchesPreviousDispatch && !dispatch.constantData.empty()) {
                    uploads[index] = dispatch.constantData;
                }
            }
            return uploads;
        }

        nrd::CommonSettings makeNrdCommonSettings(const NrdFrameParameters& parameters,
                                                  const NrdHistoryPlan& historyPlan) {
            if (!parameters.sequence.isValid() || parameters.sequence != historyPlan.sequence ||
                parameters.extent.isEmpty() || parameters.extent.width > std::numeric_limits<std::uint16_t>::max() ||
                parameters.extent.height > std::numeric_limits<std::uint16_t>::max() ||
                !finiteMatrix(parameters.camera.viewToClip) || !finiteMatrix(parameters.camera.viewToClipPrevious) ||
                !finiteMatrix(parameters.camera.worldToView) || !finiteMatrix(parameters.camera.worldToViewPrevious) ||
                !validJitter(parameters.camera.jitter) || !validJitter(parameters.camera.jitterPrevious) ||
                !std::isfinite(parameters.timeDeltaMilliseconds) || parameters.timeDeltaMilliseconds < 0.0F ||
                !std::isfinite(parameters.denoisingRange) || parameters.denoisingRange <= 0.0F) {
                throw std::invalid_argument("NRD CommonSettings inputs are invalid.");
            }

            nrd::CommonSettings settings{};
            std::ranges::copy(parameters.camera.viewToClip, settings.viewToClipMatrix);
            std::ranges::copy(parameters.camera.viewToClipPrevious, settings.viewToClipMatrixPrev);
            std::ranges::copy(parameters.camera.worldToView, settings.worldToViewMatrix);
            std::ranges::copy(parameters.camera.worldToViewPrevious, settings.worldToViewMatrixPrev);
            std::ranges::copy(parameters.camera.jitter, settings.cameraJitter);
            std::ranges::copy(parameters.camera.jitterPrevious, settings.cameraJitterPrev);
            settings.resourceSize[0] = static_cast<std::uint16_t>(parameters.extent.width);
            settings.resourceSize[1] = static_cast<std::uint16_t>(parameters.extent.height);
            settings.resourceSizePrev[0] = settings.resourceSize[0];
            settings.resourceSizePrev[1] = settings.resourceSize[1];
            settings.rectSize[0] = settings.resourceSize[0];
            settings.rectSize[1] = settings.resourceSize[1];
            settings.rectSizePrev[0] = settings.resourceSize[0];
            settings.rectSizePrev[1] = settings.resourceSize[1];
            settings.motionVectorScale[0] = 1.0F;
            settings.motionVectorScale[1] = 1.0F;
            settings.motionVectorScale[2] = 0.0F;
            settings.isMotionVectorInWorldSpace = false;
            settings.timeDeltaBetweenFrames = parameters.timeDeltaMilliseconds;
            settings.denoisingRange = parameters.denoisingRange;
            settings.viewZScale = 1.0F;
            settings.frameIndex = historyPlan.frameIndex;
            settings.accumulationMode = historyPlan.accumulationMode;
            return settings;
        }

        FrameGraphPassHandle addNrdDispatchPass(FrameGraph& frameGraph, std::string name,
                                                std::span<const NrdDispatchGraphBinding> resources,
                                                FrameGraphResourceHandle constantBuffer, bool writesConstantBuffer,
                                                FrameGraphPassHandle dependency, FrameGraph::ExecuteCallback execute) {
            if (name.empty()) {
                throw std::invalid_argument("NRD FrameGraph pass name cannot be empty.");
            }
            std::vector<NrdDispatchGraphBinding> stableResources(resources.begin(), resources.end());
            return frameGraph.addPass(
                std::move(name), FrameGraphPassType::Compute,
                [stableResources, constantBuffer, writesConstantBuffer, dependency](FrameGraphBuilder& builder) {
                    if (dependency.isValid()) {
                        builder.dependsOn(dependency);
                    }
                    if (constantBuffer.isValid()) {
                        if (writesConstantBuffer) {
                            builder.readWrite(constantBuffer, nvrhi::ResourceStates::ConstantBuffer);
                        } else {
                            builder.read(constantBuffer, nvrhi::ResourceStates::ConstantBuffer);
                        }
                    }
                    for (const NrdDispatchGraphBinding& binding : stableResources) {
                        if (!binding.resource.isValid()) {
                            throw std::invalid_argument("NRD dispatch references an invalid graph resource.");
                        }
                        if (binding.descriptorType == nrd::DescriptorType::TEXTURE) {
                            builder.readTexture(binding.resource, nvrhi::ResourceStates::ShaderResource);
                        } else if (binding.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE) {
                            builder.readWrite(binding.resource, nvrhi::ResourceStates::UnorderedAccess);
                        } else {
                            throw std::invalid_argument("NRD graph binding descriptor type is invalid.");
                        }
                    }
                },
                std::move(execute));
        }

    } // namespace detail

    struct NrdDenoiser::Impl {
        struct PipelineResources {
            nvrhi::ShaderHandle shader;
            nvrhi::BindingLayoutHandle resourceLayout;
            nvrhi::ComputePipelineHandle pipeline;
        };

        struct FrameSlotResources {
            nvrhi::BufferHandle constants;
            nvrhi::BindingSetHandle constantsAndSamplers;
            std::vector<nvrhi::BindingSetHandle> dispatchResourceSets;
        };

        struct PendingFrame {
            std::uint64_t token = 0;
            core::RenderSequence sequence;
        };

        nvrhi::IDevice& device;
        core::RenderExtent extent;
        nrd::ReblurSettings reblurSettings;
        nrd::Instance* instance = nullptr;
        const nrd::LibraryDesc& libraryDesc;
        NrdOutputResources outputResources;
        std::vector<nvrhi::TextureHandle> permanentPool;
        std::vector<nvrhi::TextureHandle> transientPool;
        std::vector<nvrhi::SamplerHandle> samplers;
        nvrhi::BindingLayoutHandle constantsAndSamplersLayout;
        std::vector<PipelineResources> pipelines;
        std::vector<FrameSlotResources> frameSlots;
        NrdHistoryTracker history;
        std::optional<PendingFrame> pending;
        std::uint64_t nextToken = 1;
        bool resourcesInitialized = false;
        bool forceClearNextFrame = true;

        explicit Impl(const NrdDenoiserCreateInfo& createInfo)
            : device(*createInfo.device), extent(createInfo.extent), reblurSettings(createInfo.reblurSettings),
              libraryDesc(*nrd::GetLibraryDesc()) {
            if (device.getGraphicsAPI() != nvrhi::GraphicsAPI::VULKAN) {
                throw std::invalid_argument("NRD adapter requires NvRHI's Vulkan backend.");
            }
            createCpuInstance();
            const nrd::InstanceDesc& instanceDesc = *nrd::GetInstanceDesc(*instance);
            createTextures(instanceDesc);
            createSamplers(instanceDesc);
            createPipelines(instanceDesc);
            createFrameSlots(instanceDesc, createInfo.frameSlotCount);
        }

        ~Impl() {
            if (instance != nullptr) {
                nrd::DestroyInstance(*instance);
            }
        }

        void createCpuInstance() {
            const nrd::DenoiserDesc denoiserDesc{reblurIdentifier, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR};
            nrd::InstanceCreationDesc creationDesc{};
            creationDesc.denoisers = &denoiserDesc;
            creationDesc.denoisersNum = 1;
            requireNrdSuccess(nrd::CreateInstance(creationDesc, instance), "nrd::CreateInstance");
            try {
                requireNrdSuccess(nrd::SetDenoiserSettings(*instance, reblurIdentifier, &reblurSettings),
                                  "nrd::SetDenoiserSettings");
            } catch (...) {
                nrd::DestroyInstance(*instance);
                instance = nullptr;
                throw;
            }
        }

        void recreateCpuInstance() {
            forceClearNextFrame = true;
            if (instance != nullptr) {
                nrd::DestroyInstance(*instance);
                instance = nullptr;
            }
            createCpuInstance();
            const nrd::InstanceDesc& newDesc = *nrd::GetInstanceDesc(*instance);
            if (newDesc.pipelinesNum != pipelines.size() || newDesc.permanentPoolSize != permanentPool.size() ||
                newDesc.transientPoolSize != transientPool.size()) {
                nrd::DestroyInstance(*instance);
                instance = nullptr;
                throw std::runtime_error("Recreated NRD instance changed its immutable GPU resource ABI.");
            }
        }

        void createTextures(const nrd::InstanceDesc& instanceDesc) {
            if (instanceDesc.permanentPoolSize > std::numeric_limits<std::uint16_t>::max() ||
                instanceDesc.transientPoolSize > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("NRD pool size exceeds its uint16 resource index ABI.");
            }
            permanentPool.reserve(instanceDesc.permanentPoolSize);
            for (std::uint32_t index = 0; index < instanceDesc.permanentPoolSize; ++index) {
                const std::string name = "NRD permanent pool " + std::to_string(index);
                const auto plan =
                    detail::makeNrdPoolTexturePlan(detail::NrdPoolKind::Permanent, static_cast<std::uint16_t>(index),
                                                   instanceDesc.permanentPool[index], extent, name.c_str());
                permanentPool.push_back(createTexture(device, plan.texture));
            }
            transientPool.reserve(instanceDesc.transientPoolSize);
            for (std::uint32_t index = 0; index < instanceDesc.transientPoolSize; ++index) {
                const std::string name = "NRD transient pool " + std::to_string(index);
                const auto plan =
                    detail::makeNrdPoolTexturePlan(detail::NrdPoolKind::Transient, static_cast<std::uint16_t>(index),
                                                   instanceDesc.transientPool[index], extent, name.c_str());
                transientPool.push_back(createTexture(device, plan.texture));
            }
            outputResources.diffuseRadianceHitDistance =
                createTexture(device, makeOutputTextureDesc(extent, "NRD diffuse radiance hit distance"));
            outputResources.specularRadianceHitDistance =
                createTexture(device, makeOutputTextureDesc(extent, "NRD specular radiance hit distance"));
        }

        void createSamplers(const nrd::InstanceDesc& instanceDesc) {
            if (instanceDesc.samplersNum != static_cast<std::uint32_t>(nrd::Sampler::MAX_NUM) ||
                instanceDesc.samplers == nullptr) {
                throw std::runtime_error("NRD InstanceDesc exposes an unsupported sampler ABI.");
            }
            samplers.reserve(instanceDesc.samplersNum);
            for (std::uint32_t index = 0; index < instanceDesc.samplersNum; ++index) {
                const nrd::Sampler sampler = instanceDesc.samplers[index];
                if (sampler != nrd::Sampler::NEAREST_CLAMP && sampler != nrd::Sampler::LINEAR_CLAMP) {
                    throw std::runtime_error("NRD requested an unsupported sampler.");
                }
                const bool linear = sampler == nrd::Sampler::LINEAR_CLAMP;
                nvrhi::SamplerDesc desc;
                desc.setAllFilters(linear).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
                nvrhi::SamplerHandle handle = device.createSampler(desc);
                if (!handle) {
                    throw std::runtime_error("Failed to create NRD sampler.");
                }
                samplers.push_back(std::move(handle));
            }
        }

        void createPipelines(const nrd::InstanceDesc& instanceDesc) {
            if (instanceDesc.pipelinesNum == 0 || instanceDesc.pipelines == nullptr ||
                instanceDesc.shaderEntryPoint == nullptr) {
                throw std::runtime_error("NRD InstanceDesc contains no compute pipelines.");
            }
            const auto firstLayouts =
                detail::makeNrdPipelineBindingLayouts(libraryDesc, instanceDesc, instanceDesc.pipelines[0]);
            constantsAndSamplersLayout =
                createBindingLayout(device, firstLayouts.constantsAndSamplers, "constants/samplers");

            pipelines.reserve(instanceDesc.pipelinesNum);
            for (std::uint32_t index = 0; index < instanceDesc.pipelinesNum; ++index) {
                const nrd::PipelineDesc& nrdPipeline = instanceDesc.pipelines[index];
                if (nrdPipeline.computeShaderSPIRV.bytecode == nullptr || nrdPipeline.computeShaderSPIRV.size == 0 ||
                    nrdPipeline.computeShaderSPIRV.size > std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error("NRD pipeline has no valid embedded SPIR-V.");
                }
                const auto layouts = detail::makeNrdPipelineBindingLayouts(libraryDesc, instanceDesc, nrdPipeline);
                PipelineResources runtime;
                runtime.resourceLayout = createBindingLayout(device, layouts.resources, "resources");

                nvrhi::ShaderDesc shaderDesc;
                shaderDesc.setShaderType(nvrhi::ShaderType::Compute)
                    .setEntryName(instanceDesc.shaderEntryPoint)
                    .setDebugName(nrdPipeline.shaderIdentifier);
                runtime.shader = device.createShader(shaderDesc, nrdPipeline.computeShaderSPIRV.bytecode,
                                                     static_cast<std::size_t>(nrdPipeline.computeShaderSPIRV.size));
                if (!runtime.shader) {
                    throw std::runtime_error("Failed to create NRD embedded SPIR-V shader.");
                }

                nvrhi::ComputePipelineDesc pipelineDesc;
                pipelineDesc.setComputeShader(runtime.shader)
                    .addBindingLayout(runtime.resourceLayout)
                    .addBindingLayout(constantsAndSamplersLayout);
                runtime.pipeline = device.createComputePipeline(pipelineDesc);
                if (!runtime.pipeline) {
                    throw std::runtime_error("Failed to create NRD compute pipeline.");
                }
                pipelines.push_back(std::move(runtime));
            }
        }

        void createFrameSlots(const nrd::InstanceDesc& instanceDesc, std::uint32_t frameSlotCount) {
            const std::uint32_t versions = std::max(instanceDesc.descriptorPoolDesc.setsMaxNum, 1U);
            const std::uint64_t byteSize = std::max(instanceDesc.constantBufferMaxDataSize, 16U);
            frameSlots.resize(frameSlotCount);
            for (std::uint32_t slotIndex = 0; slotIndex < frameSlotCount; ++slotIndex) {
                FrameSlotResources& slot = frameSlots[slotIndex];
                nvrhi::BufferDesc bufferDesc;
                bufferDesc.byteSize = byteSize;
                bufferDesc.debugName = "NRD constants frame slot " + std::to_string(slotIndex);
                bufferDesc.isConstantBuffer = true;
                bufferDesc.isVolatile = true;
                bufferDesc.maxVersions = versions;
                bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
                bufferDesc.keepInitialState = false;
                slot.constants = device.createBuffer(bufferDesc);
                if (!slot.constants) {
                    throw std::runtime_error("Failed to create NRD volatile constant buffer.");
                }

                nvrhi::BindingSetDesc bindingSetDesc;
                bindingSetDesc.addItem(
                    nvrhi::BindingSetItem::ConstantBuffer(instanceDesc.constantBufferRegisterIndex, slot.constants));
                for (std::uint32_t index = 0; index < samplers.size(); ++index) {
                    bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(
                        instanceDesc.samplersBaseRegisterIndex + index, samplers[index]));
                }
                slot.constantsAndSamplers = device.createBindingSet(bindingSetDesc, constantsAndSamplersLayout);
                if (!slot.constantsAndSamplers) {
                    throw std::runtime_error("Failed to create NRD constants/samplers binding set.");
                }
            }
        }

        [[nodiscard]] NrdTextureBinding userBinding(nrd::ResourceType type, const NrdSignalBindings& signals,
                                                    const NrdGraphOutputs& outputs) const {
            switch (type) {
            case nrd::ResourceType::IN_MV:
                return signals.motion;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                return signals.normalRoughness;
            case nrd::ResourceType::IN_VIEWZ:
                return signals.viewZ;
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                return signals.diffuseRadianceHitDistance;
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                return signals.specularRadianceHitDistance;
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                return {outputResources.diffuseRadianceHitDistance, outputs.diffuseRadianceHitDistance};
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                return {outputResources.specularRadianceHitDistance, outputs.specularRadianceHitDistance};
            default:
                throw std::invalid_argument(
                    "NRD dispatch requested an application resource not supplied by this adapter.");
            }
        }
    };

    NrdDenoiser::NrdDenoiser(const NrdDenoiserCreateInfo& createInfo) {
        if (createInfo.device == nullptr || createInfo.extent.isEmpty() || createInfo.frameSlotCount == 0 ||
            createInfo.extent.width > std::numeric_limits<std::uint16_t>::max() ||
            createInfo.extent.height > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("NRD create info requires a device, uint16 extent, and frame slots.");
        }
        impl_ = std::make_unique<Impl>(createInfo);
    }

    NrdDenoiser::~NrdDenoiser() = default;

    NrdPreparedFrame NrdDenoiser::record(FrameGraph& frameGraph, const NrdFrameParameters& parameters,
                                         const NrdSignalBindings& signals) {
        if (impl_->pending) {
            throw std::logic_error("NRD already has a pending frame.");
        }
        if (impl_->instance == nullptr) {
            impl_->recreateCpuInstance();
        }
        if (!parameters.frameSlot.isValid() || parameters.frameSlot.value() >= impl_->frameSlots.size() ||
            !parameters.frameSlotFenceWaited || parameters.extent != impl_->extent) {
            throw std::invalid_argument("NRD recording requires a waited frame slot and the configured extent.");
        }
        validateSignals(signals, impl_->extent, impl_->libraryDesc);

        NrdHistoryPlan historyPlan = impl_->history.beginFrame(
            parameters.sequence, parameters.diffuseHistory, parameters.specularHistory, parameters.cameraCut,
            parameters.renderResourcesRecreated || impl_->forceClearNextFrame);
        bool dispatchStateAdvanced = false;
        try {
            const nrd::CommonSettings commonSettings = detail::makeNrdCommonSettings(parameters, historyPlan);
            requireNrdSuccess(nrd::SetCommonSettings(*impl_->instance, commonSettings), "nrd::SetCommonSettings");

            const nrd::DispatchDesc* dispatchDescs = nullptr;
            std::uint32_t dispatchCount = 0;
            requireNrdSuccess(
                nrd::GetComputeDispatches(*impl_->instance, &reblurIdentifier, 1, dispatchDescs, dispatchCount),
                "nrd::GetComputeDispatches");
            dispatchStateAdvanced = true;
            const std::vector<detail::NrdDispatchPlan> plans =
                detail::makeNrdDispatchPlans(*nrd::GetInstanceDesc(*impl_->instance),
                                             std::span<const nrd::DispatchDesc>(dispatchDescs, dispatchCount));
            const std::vector<std::vector<std::uint8_t>> constantUploads = detail::makeNrdConstantUploads(plans);
            const std::size_t constantWrites =
                std::ranges::count_if(constantUploads, [](const std::vector<std::uint8_t>& upload) {
                    return !upload.empty();
                });
            if (constantWrites > impl_->frameSlots[parameters.frameSlot.value()].constants->getDesc().maxVersions) {
                throw std::runtime_error("NRD dispatches exceed the volatile constant-buffer version capacity.");
            }

            std::vector<FrameGraphResourceHandle> permanentGraph;
            permanentGraph.reserve(impl_->permanentPool.size());
            for (std::uint32_t index = 0; index < impl_->permanentPool.size(); ++index) {
                permanentGraph.push_back(frameGraph.importTexture(
                    "nrd.permanent." + std::to_string(index),
                    importedTextureDesc(impl_->permanentPool[index], impl_->resourcesInitialized)));
            }
            std::vector<FrameGraphResourceHandle> transientGraph;
            transientGraph.reserve(impl_->transientPool.size());
            for (std::uint32_t index = 0; index < impl_->transientPool.size(); ++index) {
                transientGraph.push_back(frameGraph.importTexture(
                    "nrd.transient." + std::to_string(index),
                    importedTextureDesc(impl_->transientPool[index], impl_->resourcesInitialized)));
            }

            NrdGraphOutputs graphOutputs;
            graphOutputs.diffuseRadianceHitDistance = frameGraph.importTexture(
                "nrd.output.diffuse",
                importedTextureDesc(impl_->outputResources.diffuseRadianceHitDistance, impl_->resourcesInitialized));
            graphOutputs.specularRadianceHitDistance = frameGraph.importTexture(
                "nrd.output.specular",
                importedTextureDesc(impl_->outputResources.specularRadianceHitDistance, impl_->resourcesInitialized));

            Impl::FrameSlotResources& slot = impl_->frameSlots[parameters.frameSlot.value()];
            slot.dispatchResourceSets.clear();
            slot.dispatchResourceSets.reserve(plans.size());
            const FrameGraphResourceHandle constantBuffer =
                frameGraph.importBuffer("nrd.constants." + std::to_string(parameters.frameSlot.value()),
                                        FrameGraphBufferDesc{
                                            slot.constants->getDesc().byteSize,
                                            slot.constants,
                                            nvrhi::ResourceStates::ConstantBuffer,
                                            nvrhi::ResourceStates::ConstantBuffer,
                                        });

            NrdPreparedFrame prepared;
            prepared.token_ = impl_->nextToken++;
            if (prepared.token_ == 0) {
                prepared.token_ = impl_->nextToken++;
            }
            prepared.sequence_ = parameters.sequence;
            prepared.frameSlot_ = parameters.frameSlot;
            prepared.historyPlan_ = historyPlan;
            prepared.outputs_ = graphOutputs;
            prepared.passes_.reserve(plans.size());

            FrameGraphPassHandle previousPass;
            for (std::size_t dispatchIndex = 0; dispatchIndex < plans.size(); ++dispatchIndex) {
                const detail::NrdDispatchPlan& plan = plans[dispatchIndex];
                const Impl::PipelineResources& pipeline = impl_->pipelines.at(plan.pipelineIndex);
                nvrhi::BindingSetDesc bindingSetDesc;
                std::vector<detail::NrdDispatchGraphBinding> graphBindings;
                graphBindings.reserve(plan.resources.size());
                for (const detail::NrdDispatchResourcePlan& resourcePlan : plan.resources) {
                    NrdTextureBinding binding;
                    switch (resourcePlan.resource.source) {
                    case detail::NrdResourceSource::User:
                        binding = impl_->userBinding(resourcePlan.resource.userType, signals, graphOutputs);
                        break;
                    case detail::NrdResourceSource::PermanentPool:
                        binding = {impl_->permanentPool.at(resourcePlan.resource.poolIndex),
                                   permanentGraph.at(resourcePlan.resource.poolIndex)};
                        break;
                    case detail::NrdResourceSource::TransientPool:
                        binding = {impl_->transientPool.at(resourcePlan.resource.poolIndex),
                                   transientGraph.at(resourcePlan.resource.poolIndex)};
                        break;
                    }
                    if (!complete(binding)) {
                        throw std::logic_error("Resolved NRD dispatch resource is incomplete.");
                    }
                    if (resourcePlan.descriptorType == nrd::DescriptorType::TEXTURE) {
                        bindingSetDesc.addItem(
                            nvrhi::BindingSetItem::Texture_SRV(resourcePlan.bindingSlot, binding.texture));
                    } else {
                        bindingSetDesc.addItem(
                            nvrhi::BindingSetItem::Texture_UAV(resourcePlan.bindingSlot, binding.texture));
                    }
                    graphBindings.push_back({binding.graphResource, resourcePlan.descriptorType});
                }

                nvrhi::BindingSetHandle resourceSet =
                    impl_->device.createBindingSet(bindingSetDesc, pipeline.resourceLayout);
                if (!resourceSet) {
                    throw std::runtime_error("Failed to create NRD dispatch resource binding set.");
                }
                slot.dispatchResourceSets.push_back(resourceSet);

                const nvrhi::ComputePipelineHandle stablePipeline = pipeline.pipeline;
                const nvrhi::BindingSetHandle stableResourceSet = resourceSet;
                const nvrhi::BindingSetHandle stableConstantsSet = slot.constantsAndSamplers;
                const nvrhi::BufferHandle stableConstants = slot.constants;
                const std::vector<std::uint8_t> constantUpload = constantUploads[dispatchIndex];
                const bool writeConstants = !constantUpload.empty();
                const std::uint32_t gridWidth = plan.gridWidth;
                const std::uint32_t gridHeight = plan.gridHeight;
                const std::string passName = "nrd.reblur." + std::to_string(dispatchIndex) + "." + plan.name;
                previousPass = detail::addNrdDispatchPass(
                    frameGraph, passName, graphBindings, constantBuffer, writeConstants, previousPass,
                    [stablePipeline, stableResourceSet, stableConstantsSet, stableConstants, constantUpload, gridWidth,
                     gridHeight, writeConstants](const FrameGraphContext& context) {
                        if (context.commandList == nullptr) {
                            throw std::logic_error("NRD dispatch requires an NvRHI command list.");
                        }
                        if (writeConstants) {
                            context.commandList->writeBuffer(stableConstants, constantUpload.data(),
                                                             constantUpload.size());
                        }
                        nvrhi::ComputeState state;
                        state.setPipeline(stablePipeline)
                            .addBindingSet(stableResourceSet)
                            .addBindingSet(stableConstantsSet);
                        detail::recordNrdDispatch(*context.commandList, state, gridWidth, gridHeight);
                    });
                prepared.passes_.push_back(previousPass);
            }

            impl_->pending = Impl::PendingFrame{prepared.token_, parameters.sequence};
            return prepared;
        } catch (...) {
            const std::exception_ptr recordingFailure = std::current_exception();
            if (impl_->history.hasActiveFrame()) {
                impl_->history.discardFrame(parameters.sequence);
            }
            if (dispatchStateAdvanced) {
                impl_->recreateCpuInstance();
            }
            std::rethrow_exception(recordingFailure);
        }
    }

    void NrdDenoiser::commitSubmittedFrame(const NrdPreparedFrame& frame) {
        if (!frame.isValid() || !impl_->pending || impl_->pending->token != frame.token_ ||
            impl_->pending->sequence != frame.sequence_) {
            throw std::logic_error("NRD submit ticket does not match the pending frame.");
        }
        impl_->history.commitSubmittedFrame(frame.sequence_);
        impl_->resourcesInitialized = true;
        impl_->forceClearNextFrame = false;
        impl_->pending.reset();
    }

    void NrdDenoiser::discardFrame(const NrdPreparedFrame& frame) noexcept {
        if (!frame.isValid() || !impl_->pending || impl_->pending->token != frame.token_ ||
            impl_->pending->sequence != frame.sequence_) {
            return;
        }
        discardPendingFrame();
    }

    void NrdDenoiser::discardPendingFrame() noexcept {
        if (!impl_->pending) {
            return;
        }
        const core::RenderSequence sequence = impl_->pending->sequence;
        if (impl_->history.hasActiveFrame()) {
            try {
                impl_->history.discardFrame(sequence);
            } catch (...) {
                // 异常清理不能覆盖原始渲染失败；下一次 record 会重新校验并重建 adapter。
            }
        }
        impl_->pending.reset();
        try {
            impl_->recreateCpuInstance();
        } catch (...) {
            // instance 保持为空，下一次 record 会同步重试；已提交历史与 GPU pool 均未推进。
        }
    }

    const NrdOutputResources& NrdDenoiser::outputs() const noexcept {
        return impl_->outputResources;
    }

    nvrhi::Format NrdDenoiser::expectedNormalRoughnessFormat() const noexcept {
        try {
            return detail::nrdNormalEncodingFormat(impl_->libraryDesc.normalEncoding);
        } catch (...) {
            return nvrhi::Format::UNKNOWN;
        }
    }

    const NrdHistoryState& NrdDenoiser::historyState(core::HistoryDomain domain) const {
        return impl_->history.state(domain);
    }

    bool NrdDenoiser::hasPendingFrame() const noexcept {
        return impl_->pending.has_value();
    }

} // namespace lumin::render::gi

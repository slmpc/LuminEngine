#include "render/features/postfx/PostFxResources.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace lumin::render {
    namespace {

        [[nodiscard]] bool supports(nvrhi::FormatSupport available, nvrhi::FormatSupport required) {
            return (available & required) == required;
        }

        [[nodiscard]] nvrhi::TextureDesc textureDesc(std::uint32_t width, std::uint32_t height, nvrhi::Format format,
                                                     const char* debugName) {
            nvrhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.debugName = debugName;
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            return desc;
        }

    } // namespace

    PostFxResources::PostFxResources(nvrhi::IDevice& device, std::uint32_t frameSlotCount)
        : device_(device), resources_(device), frames_(frameSlotCount), bindingSets_(frameSlotCount),
          historyValid_(frameSlotCount, false), historyInitialized_(frameSlotCount, false) {
        if (frameSlotCount == 0) {
            throw std::invalid_argument("PostFX resources require at least one frame slot.");
        }
    }

    PostFxResources::~PostFxResources() {
        destroy();
    }

    void PostFxResources::create(std::uint32_t width, std::uint32_t height,
                                 std::span<const PostFxBindingInputs> inputs) {
        destroy();
        if (width == 0 || height == 0) {
            throw std::invalid_argument("PostFX resources require a non-zero render extent.");
        }
        if (inputs.size() != frames_.size()) {
            throw std::invalid_argument("PostFX binding inputs must match the configured frame-slot count.");
        }
        for (const PostFxBindingInputs& input : inputs) {
            for (const nvrhi::TextureHandle& texture : input.surfaces) {
                if (!texture) {
                    throw std::invalid_argument("PostFX binding inputs require complete surface textures.");
                }
            }
            for (const nvrhi::TextureHandle& texture : input.shadows) {
                if (!texture) {
                    throw std::invalid_argument("PostFX binding inputs require complete shadow textures.");
                }
            }
        }
        try {
            createImages(width, height);
            createSamplerAndBindings(inputs);
            createBloomBindings();
        } catch (...) {
            destroy();
            throw;
        }
    }

    void PostFxResources::destroy() noexcept {
        std::ranges::fill(bindingSets_, nullptr);
        // 描述符与 framebuffer 可能强引用跨帧槽资源，必须在任一底层纹理前统一释放。
        for (PostFxFrameResources& frameResources : frames_) {
            frameResources.bloomCompositeBinding = nullptr;
            frameResources.bloomUpsampleBindings.fill(nullptr);
            frameResources.bloomDownsampleBindings.fill(nullptr);
            frameResources.bloomOutputFramebuffer = nullptr;
            frameResources.bloomUpsampleFramebuffers.fill(nullptr);
            frameResources.bloomDownsampleFramebuffers.fill(nullptr);
        }
        bloomBindingLayout_ = nullptr;
        bindingLayout_ = nullptr;
        sampler_ = nullptr;
        for (PostFxFrameResources& frameResources : frames_) {
            resources_.destroyBuffer(frameResources.uniforms);
            resources_.destroyTexture(frameResources.bloomOutput);
            for (GpuTexture& texture : frameResources.bloomUpsample) {
                resources_.destroyTexture(texture);
            }
            for (GpuTexture& texture : frameResources.bloomDownsample) {
                resources_.destroyTexture(texture);
            }
            resources_.destroyTexture(frameResources.history);
            resources_.destroyTexture(frameResources.taaResolved);
            resources_.destroyTexture(frameResources.lighting);
            resources_.destroyTexture(frameResources.directRadiance);
            resources_.destroyTexture(frameResources.denoisedDirectRadiance);
            resources_.destroyTexture(frameResources.globalIllumination);
        }
        globalIlluminationFormat_ = nvrhi::Format::UNKNOWN;
        lightingFormat_ = nvrhi::Format::UNKNOWN;
        std::ranges::fill(historyValid_, false);
        std::ranges::fill(historyInitialized_, false);
    }

    void PostFxResources::updateUniforms(std::uint32_t frameIndex, const PostProcessUniforms& uniforms) {
        if (frameIndex >= frames_.size()) {
            throw std::out_of_range("PostFX uniform frame index is out of range.");
        }
        resources_.writeBuffer(frames_[frameIndex].uniforms, &uniforms, sizeof(uniforms));
    }

    void PostFxResources::invalidateHistory() noexcept {
        std::ranges::fill(historyValid_, false);
    }

    void PostFxResources::markHistoryValid(std::uint32_t frameIndex) {
        if (frameIndex >= frames_.size()) {
            throw std::out_of_range("PostFX history frame index is out of range.");
        }
        historyValid_[frameIndex] = true;
        historyInitialized_[frameIndex] = true;
        frames_[frameIndex].history.initialState = nvrhi::ResourceStates::ShaderResource;
    }

    const PostFxFrameResources& PostFxResources::frame(std::uint32_t frameIndex) const {
        if (frameIndex >= frames_.size()) {
            throw std::out_of_range("PostFX frame index is out of range.");
        }
        return frames_[frameIndex];
    }

    std::uint32_t PostFxResources::frameSlotCount() const noexcept {
        return static_cast<std::uint32_t>(frames_.size());
    }

    bool PostFxResources::historyValid(std::uint32_t frameIndex) const {
        if (frameIndex >= historyValid_.size()) {
            throw std::out_of_range("PostFX history frame index is out of range.");
        }
        return historyValid_[frameIndex];
    }

    bool PostFxResources::historyInitialized(std::uint32_t frameIndex) const {
        if (frameIndex >= historyInitialized_.size()) {
            throw std::out_of_range("PostFX history frame index is out of range.");
        }
        return historyInitialized_[frameIndex];
    }

    nvrhi::ResourceStates PostFxResources::historyInitialState(std::uint32_t frameIndex) const {
        return historyInitialized(frameIndex) ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
    }

    nvrhi::Format PostFxResources::lightingFormat() const noexcept {
        return lightingFormat_;
    }

    nvrhi::Format PostFxResources::globalIlluminationFormat() const noexcept {
        return globalIlluminationFormat_;
    }

    nvrhi::SamplerHandle PostFxResources::sampler() const noexcept {
        return sampler_;
    }

    nvrhi::BindingLayoutHandle PostFxResources::bindingLayout() const noexcept {
        return bindingLayout_;
    }

    nvrhi::BindingLayoutHandle PostFxResources::bloomBindingLayout() const noexcept {
        return bloomBindingLayout_;
    }

    nvrhi::BindingSetHandle PostFxResources::bindingSet(std::uint32_t frameIndex) const {
        if (frameIndex >= bindingSets_.size()) {
            throw std::out_of_range("PostFX binding frame index is out of range.");
        }
        return bindingSets_[frameIndex];
    }

    nvrhi::Format PostFxResources::chooseFormat(std::span<const nvrhi::Format> candidates,
                                                nvrhi::FormatSupport required) const {
        for (const nvrhi::Format candidate : candidates) {
            if (supports(device_.queryFormatSupport(candidate), required)) {
                return candidate;
            }
        }
        throw std::runtime_error("No supported NvRHI format satisfies the PostFX usage.");
    }

    GpuTexture PostFxResources::createTexture(const nvrhi::TextureDesc& desc) const {
        GpuTexture result;
        result.texture = device_.createTexture(desc);
        if (!result.texture) {
            throw std::runtime_error("Failed to create a PostFX texture.");
        }
        result.format = desc.format;
        result.width = desc.width;
        result.height = desc.height;
        result.mipLevels = desc.mipLevels;
        result.arrayLayers = desc.arraySize;
        result.initialState = desc.initialState;
        return result;
    }

    void PostFxResources::createImages(std::uint32_t width, std::uint32_t height) {
        const nvrhi::FormatSupport colorSampled =
            nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderSample;
        const nvrhi::FormatSupport storageSampled =
            colorSampled | nvrhi::FormatSupport::ShaderUavLoad | nvrhi::FormatSupport::ShaderUavStore;
        constexpr std::array colorCandidates = {nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA32_FLOAT};
        globalIlluminationFormat_ = chooseFormat(colorCandidates, storageSampled);
        lightingFormat_ = chooseFormat(colorCandidates, storageSampled);

        for (PostFxFrameResources& frameResources : frames_) {
            nvrhi::TextureDesc desc = textureDesc(width, height, globalIlluminationFormat_, "Global illumination");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.globalIllumination = createTexture(desc);

            desc = textureDesc(width, height, globalIlluminationFormat_, "RT direct radiance");
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.directRadiance = createTexture(desc);

            desc = textureDesc(width, height, globalIlluminationFormat_, "NRD direct radiance");
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.denoisedDirectRadiance = createTexture(desc);

            desc = textureDesc(width, height, lightingFormat_, "Scene HDR lighting");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.lighting = createTexture(desc);

            desc = textureDesc(width, height, lightingFormat_, "TAA resolved");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.taaResolved = createTexture(desc);

            desc = textureDesc(width, height, lightingFormat_, "TAA history");
            desc.isShaderResource = true;
            frameResources.history = createTexture(desc);

            std::uint32_t bloomWidth = width;
            std::uint32_t bloomHeight = height;
            for (GpuTexture& texture : frameResources.bloomDownsample) {
                bloomWidth = std::max((bloomWidth + 1U) / 2U, 1U);
                bloomHeight = std::max((bloomHeight + 1U) / 2U, 1U);
                desc = textureDesc(bloomWidth, bloomHeight, lightingFormat_, "Bloom downsample");
                desc.isRenderTarget = true;
                desc.isShaderResource = true;
                texture = createTexture(desc);
            }
            for (std::uint32_t level = 0; level < frameResources.bloomUpsample.size(); ++level) {
                const GpuTexture& downsample = frameResources.bloomDownsample[level];
                desc = textureDesc(downsample.width, downsample.height, lightingFormat_, "Bloom upsample");
                desc.isRenderTarget = true;
                desc.isShaderResource = true;
                frameResources.bloomUpsample[level] = createTexture(desc);
            }
            desc = textureDesc(width, height, lightingFormat_, "Bloom HDR output");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            frameResources.bloomOutput = createTexture(desc);

            nvrhi::BufferDesc bufferDesc;
            bufferDesc.byteSize = sizeof(PostProcessUniforms);
            bufferDesc.debugName = "Post-process uniforms";
            bufferDesc.isConstantBuffer = true;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
            frameResources.uniforms = resources_.createBuffer(bufferDesc);
        }
        std::ranges::fill(historyValid_, false);
        std::ranges::fill(historyInitialized_, false);
    }

    void PostFxResources::createSamplerAndBindings(std::span<const PostFxBindingInputs> inputs) {
        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setMinFilter(true).setMagFilter(true).setMipFilter(false);
        samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        sampler_ = device_.createSampler(samplerDesc);
        if (!sampler_) {
            throw std::runtime_error("Failed to create the fullscreen sampler.");
        }

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.setVisibility(nvrhi::ShaderType::Pixel).setRegisterSpaceAndDescriptorSet(0);
        layoutDesc.bindingOffsets.setShaderResourceOffset(0).setSamplerOffset(0).setConstantBufferOffset(0);
        for (std::uint32_t binding = 0; binding < fullscreenSampledImageCount; ++binding) {
            layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(binding));
        }
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(fullscreenSamplerBinding));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(fullscreenUniformBinding));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(fullscreenBloomBinding));
        bindingLayout_ = device_.createBindingLayout(layoutDesc);
        if (!bindingLayout_) {
            throw std::runtime_error("Failed to create the fullscreen binding layout.");
        }

        for (std::uint32_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex) {
            const PostFxFrameResources& frameResources = frames_[frameIndex];
            const PostFxFrameResources& previousFrame = frames_[(frameIndex + frames_.size() - 1) % frames_.size()];
            nvrhi::BindingSetDesc bindingDesc;
            for (std::uint32_t binding = 0; binding < inputs[frameIndex].surfaces.size(); ++binding) {
                bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(binding, inputs[frameIndex].surfaces[binding]));
            }
            const std::array postFxTextures = {
                frameResources.globalIllumination.texture,
                frameResources.lighting.texture,
                previousFrame.history.texture,
                frameResources.taaResolved.texture,
            };
            for (std::uint32_t index = 0; index < postFxTextures.size(); ++index) {
                bindingDesc.addItem(
                    nvrhi::BindingSetItem::Texture_SRV(fullscreenSurfaceImageCount + index, postFxTextures[index]));
            }
            for (std::uint32_t cascade = 0; cascade < inputs[frameIndex].shadows.size(); ++cascade) {
                bindingDesc.addItem(
                    nvrhi::BindingSetItem::Texture_SRV(8 + cascade, inputs[frameIndex].shadows[cascade]));
            }
            bindingDesc.addItem(nvrhi::BindingSetItem::Sampler(fullscreenSamplerBinding, sampler_));
            bindingDesc.addItem(
                nvrhi::BindingSetItem::ConstantBuffer(fullscreenUniformBinding, frameResources.uniforms.buffer));
            bindingDesc.addItem(
                nvrhi::BindingSetItem::Texture_SRV(fullscreenBloomBinding, frameResources.bloomOutput.texture));
            bindingSets_[frameIndex] = device_.createBindingSet(bindingDesc, bindingLayout_);
            if (!bindingSets_[frameIndex]) {
                throw std::runtime_error("Failed to create a fullscreen binding set.");
            }
        }
    }

    void PostFxResources::createBloomBindings() {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.setVisibility(nvrhi::ShaderType::Pixel).setRegisterSpaceAndDescriptorSet(0);
        layoutDesc.bindingOffsets.setShaderResourceOffset(0).setSamplerOffset(0).setConstantBufferOffset(0);
        layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BloomPushConstants)))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::Sampler(2));
        bloomBindingLayout_ = device_.createBindingLayout(layoutDesc);
        if (!bloomBindingLayout_) {
            throw std::runtime_error("Failed to create the Bloom binding layout.");
        }

        const auto createBinding = [this](nvrhi::ITexture* source, nvrhi::ITexture* base) {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomPushConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(1, base))
                .addItem(nvrhi::BindingSetItem::Sampler(2, sampler_));
            nvrhi::BindingSetHandle result = device_.createBindingSet(desc, bloomBindingLayout_);
            if (!result) {
                throw std::runtime_error("Failed to create a Bloom binding set.");
            }
            return result;
        };
        const auto createFramebuffer = [this](nvrhi::ITexture* output) {
            nvrhi::FramebufferHandle result =
                device_.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output));
            if (!result) {
                throw std::runtime_error("Failed to create a Bloom framebuffer.");
            }
            return result;
        };

        for (PostFxFrameResources& frame : frames_) {
            for (std::uint32_t level = 0; level < bloomLevelCount; ++level) {
                nvrhi::ITexture* source =
                    level == 0 ? frame.taaResolved.texture.Get() : frame.bloomDownsample[level - 1].texture.Get();
                frame.bloomDownsampleBindings[level] = createBinding(source, source);
                frame.bloomDownsampleFramebuffers[level] = createFramebuffer(frame.bloomDownsample[level].texture);
            }
            for (std::uint32_t level = 0; level < bloomUpsampleLevelCount; ++level) {
                nvrhi::ITexture* source = level + 1 == bloomLevelCount - 1
                                              ? frame.bloomDownsample[level + 1].texture.Get()
                                              : frame.bloomUpsample[level + 1].texture.Get();
                frame.bloomUpsampleBindings[level] = createBinding(source, frame.bloomDownsample[level].texture);
                frame.bloomUpsampleFramebuffers[level] = createFramebuffer(frame.bloomUpsample[level].texture);
            }
            frame.bloomCompositeBinding = createBinding(frame.bloomUpsample[0].texture, frame.taaResolved.texture);
            frame.bloomOutputFramebuffer = createFramebuffer(frame.bloomOutput.texture);
        }
    }

} // namespace lumin::render

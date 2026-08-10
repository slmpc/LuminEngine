#include "render/TextureManager.hpp"

#if !defined(LUMIN_TEXTURE_MANAGER_STANDALONE_TEST)
#include "render/VulkanContext.hpp"
#endif

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

#if !defined(LUMIN_TEXTURE_MANAGER_STANDALONE_TEST)
    TextureManager::TextureManager(VulkanContext& context) : TextureManager(*context.rhiDevice().Get()) {
    }
#endif

    TextureManager::TextureManager(nvrhi::IDevice& device) : device_(device), resources_(device) {
    }

    TextureManager::~TextureManager() {
        destroy();
    }

    void TextureManager::create(std::uint32_t width, std::uint32_t height) {
        destroy();
        if (width == 0 || height == 0) {
            throw std::invalid_argument("TextureManager requires a non-zero render extent.");
        }
        try {
            createImages(width, height);
            createSamplerAndBindings();
        } catch (...) {
            destroy();
            throw;
        }
    }

    void TextureManager::destroy() noexcept {
        bindingSets_.fill(nullptr);
        bindingLayout_ = nullptr;
        sampler_ = nullptr;

        for (TextureFrameResources& frameResources : frames_) {
            resources_.destroyBuffer(frameResources.postProcessUniform);
            for (GpuTexture& shadow : frameResources.shadowCascades) {
                resources_.destroyTexture(shadow);
            }
            resources_.destroyTexture(frameResources.history);
            resources_.destroyTexture(frameResources.taaResolved);
            resources_.destroyTexture(frameResources.lighting);
            resources_.destroyTexture(frameResources.globalIllumination);
            resources_.destroyTexture(frameResources.depth);
            resources_.destroyTexture(frameResources.materialId);
            resources_.destroyTexture(frameResources.motion);
            resources_.destroyTexture(frameResources.albedo);
            resources_.destroyTexture(frameResources.normalRoughness);
            resources_.destroyTexture(frameResources.position);
        }

        positionFormat_ = nvrhi::Format::UNKNOWN;
        normalFormat_ = nvrhi::Format::UNKNOWN;
        albedoFormat_ = nvrhi::Format::UNKNOWN;
        motionFormat_ = nvrhi::Format::UNKNOWN;
        materialIdFormat_ = nvrhi::Format::UNKNOWN;
        depthFormat_ = nvrhi::Format::UNKNOWN;
        globalIlluminationFormat_ = nvrhi::Format::UNKNOWN;
        lightingFormat_ = nvrhi::Format::UNKNOWN;
        shadowDepthFormat_ = nvrhi::Format::UNKNOWN;
        historyValid_.fill(false);
        historyInitialized_.fill(false);
    }

    void TextureManager::updatePostProcessUniforms(std::uint32_t frameIndex, const PostProcessUniforms& uniforms) {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager uniform frame index is out of range.");
        }
        resources_.writeBuffer(frames_[frameIndex].postProcessUniform, &uniforms, sizeof(uniforms));
    }

    void TextureManager::invalidateHistory() noexcept {
        historyValid_.fill(false);
    }

    void TextureManager::markHistoryValid(std::uint32_t frameIndex) {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager history frame index is out of range.");
        }
        historyValid_[frameIndex] = true;
        historyInitialized_[frameIndex] = true;
        frames_[frameIndex].history.initialState = nvrhi::ResourceStates::ShaderResource;
    }

    const TextureFrameResources& TextureManager::frame(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager frame index is out of range.");
        }
        return frames_[frameIndex];
    }

    bool TextureManager::historyValid(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager history frame index is out of range.");
        }
        return historyValid_[frameIndex];
    }

    bool TextureManager::historyInitialized(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager history frame index is out of range.");
        }
        return historyInitialized_[frameIndex];
    }

    nvrhi::ResourceStates TextureManager::historyInitialState(std::uint32_t frameIndex) const {
        return historyInitialized(frameIndex) ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
    }

    nvrhi::Format TextureManager::positionFormat() const noexcept {
        return positionFormat_;
    }

    nvrhi::Format TextureManager::normalFormat() const noexcept {
        return normalFormat_;
    }

    nvrhi::Format TextureManager::albedoFormat() const noexcept {
        return albedoFormat_;
    }

    nvrhi::Format TextureManager::motionFormat() const noexcept {
        return motionFormat_;
    }

    nvrhi::Format TextureManager::materialIdFormat() const noexcept {
        return materialIdFormat_;
    }

    nvrhi::Format TextureManager::depthFormat() const noexcept {
        return depthFormat_;
    }

    nvrhi::Format TextureManager::globalIlluminationFormat() const noexcept {
        return globalIlluminationFormat_;
    }

    nvrhi::Format TextureManager::lightingFormat() const noexcept {
        return lightingFormat_;
    }

    nvrhi::Format TextureManager::shadowDepthFormat() const noexcept {
        return shadowDepthFormat_;
    }

    nvrhi::SamplerHandle TextureManager::sampler() const noexcept {
        return sampler_;
    }

    nvrhi::BindingLayoutHandle TextureManager::bindingLayout() const noexcept {
        return bindingLayout_;
    }

    nvrhi::BindingSetHandle TextureManager::bindingSet(std::uint32_t frameIndex) const {
        if (frameIndex >= maxFramesInFlight) {
            throw std::out_of_range("TextureManager binding frame index is out of range.");
        }
        return bindingSets_[frameIndex];
    }

    nvrhi::Format TextureManager::chooseFormat(std::span<const nvrhi::Format> candidates,
                                               nvrhi::FormatSupport required) const {
        for (const nvrhi::Format candidate : candidates) {
            if (supports(device_.queryFormatSupport(candidate), required)) {
                return candidate;
            }
        }
        throw std::runtime_error("No supported NvRHI texture format satisfies the requested usage.");
    }

    GpuTexture TextureManager::createTexture(const nvrhi::TextureDesc& desc) const {
        GpuTexture texture;
        texture.texture = device_.createTexture(desc);
        if (!texture.texture) {
            throw std::runtime_error("Failed to create an NvRHI render texture.");
        }
        texture.format = desc.format;
        texture.width = desc.width;
        texture.height = desc.height;
        texture.mipLevels = desc.mipLevels;
        texture.arrayLayers = desc.arraySize;
        texture.initialState = desc.initialState;
        return texture;
    }

    void TextureManager::createImages(std::uint32_t width, std::uint32_t height) {
        const nvrhi::FormatSupport colorSampled =
            nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderSample;
        const nvrhi::FormatSupport depthAttachment = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::DepthStencil;
        const nvrhi::FormatSupport depthSampled = depthAttachment | nvrhi::FormatSupport::ShaderSample;
        const nvrhi::FormatSupport storageSampled =
            colorSampled | nvrhi::FormatSupport::ShaderUavLoad | nvrhi::FormatSupport::ShaderUavStore;
        const nvrhi::FormatSupport integerStorage =
            nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderLoad |
            nvrhi::FormatSupport::ShaderUavLoad | nvrhi::FormatSupport::ShaderUavStore;

        constexpr std::array positionCandidates = {nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA32_FLOAT};
        constexpr std::array albedoCandidates = {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::RGBA16_FLOAT,
                                                 nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::BGRA8_UNORM};
        constexpr std::array motionCandidates = {nvrhi::Format::RG16_FLOAT, nvrhi::Format::RGBA16_FLOAT};
        constexpr std::array depthCandidates = {nvrhi::Format::D32, nvrhi::Format::D32S8, nvrhi::Format::D24S8};
        constexpr std::array shadowCandidates = {nvrhi::Format::D32, nvrhi::Format::D16};

        positionFormat_ = chooseFormat(positionCandidates, storageSampled);
        normalFormat_ = positionFormat_;
        albedoFormat_ = chooseFormat(albedoCandidates, storageSampled);
        motionFormat_ = chooseFormat(motionCandidates, storageSampled);
        constexpr std::array materialIdCandidates = {nvrhi::Format::R32_UINT};
        materialIdFormat_ = chooseFormat(materialIdCandidates, integerStorage);
        globalIlluminationFormat_ = chooseFormat(positionCandidates, storageSampled);
        lightingFormat_ = chooseFormat(positionCandidates, storageSampled);
        depthFormat_ = chooseFormat(depthCandidates, depthAttachment);
        shadowDepthFormat_ = chooseFormat(shadowCandidates, depthSampled);

        for (TextureFrameResources& frameResources : frames_) {
            nvrhi::TextureDesc desc = textureDesc(width, height, positionFormat_, "G-buffer position");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.position = createTexture(desc);

            desc = textureDesc(width, height, normalFormat_, "G-buffer normal roughness");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.normalRoughness = createTexture(desc);

            desc = textureDesc(width, height, albedoFormat_, "G-buffer albedo");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.albedo = createTexture(desc);

            desc = textureDesc(width, height, motionFormat_, "G-buffer motion");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.motion = createTexture(desc);

            desc = textureDesc(width, height, materialIdFormat_, "G-buffer material ID");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            desc.isUAV = true;
            frameResources.materialId = createTexture(desc);

            desc = textureDesc(width, height, depthFormat_, "G-buffer depth");
            desc.isShaderResource = false;
            desc.isRenderTarget = true;
            frameResources.depth = createTexture(desc);

            desc = textureDesc(width, height, globalIlluminationFormat_, "Global illumination");
            desc.isRenderTarget = true;
            desc.isUAV = true;
            frameResources.globalIllumination = createTexture(desc);

            desc = textureDesc(width, height, lightingFormat_, "Deferred lighting");
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
            frameResources.history = createTexture(desc);

            desc = textureDesc(shadowMapResolution, shadowMapResolution, shadowDepthFormat_, "Shadow cascade");
            desc.isRenderTarget = true;
            for (GpuTexture& shadow : frameResources.shadowCascades) {
                shadow = createTexture(desc);
            }

            nvrhi::BufferDesc bufferDesc;
            bufferDesc.byteSize = sizeof(PostProcessUniforms);
            bufferDesc.debugName = "Post-process uniforms";
            bufferDesc.isConstantBuffer = true;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
            frameResources.postProcessUniform = resources_.createBuffer(bufferDesc);
        }
        historyValid_.fill(false);
        historyInitialized_.fill(false);
    }

    void TextureManager::createSamplerAndBindings() {
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
        bindingLayout_ = device_.createBindingLayout(layoutDesc);
        if (!bindingLayout_) {
            throw std::runtime_error("Failed to create the fullscreen binding layout.");
        }

        for (std::uint32_t frameIndex = 0; frameIndex < maxFramesInFlight; ++frameIndex) {
            const TextureFrameResources& frameResources = frames_[frameIndex];
            const TextureFrameResources& previousFrame =
                frames_[(frameIndex + maxFramesInFlight - 1) % maxFramesInFlight];
            const std::array<const GpuTexture*, 8> frameTextures = {
                &frameResources.position, &frameResources.normalRoughness,    &frameResources.albedo,
                &frameResources.motion,   &frameResources.globalIllumination, &frameResources.lighting,
                &previousFrame.history,   &frameResources.taaResolved,
            };

            nvrhi::BindingSetDesc bindingDesc;
            for (std::uint32_t binding = 0; binding < frameTextures.size(); ++binding) {
                bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(binding, frameTextures[binding]->texture));
            }
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                bindingDesc.addItem(
                    nvrhi::BindingSetItem::Texture_SRV(8 + cascade, frameResources.shadowCascades[cascade].texture));
            }
            bindingDesc.addItem(nvrhi::BindingSetItem::Sampler(fullscreenSamplerBinding, sampler_));
            bindingDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(fullscreenUniformBinding,
                                                                      frameResources.postProcessUniform.buffer));
            bindingSets_[frameIndex] = device_.createBindingSet(bindingDesc, bindingLayout_);
            if (!bindingSets_[frameIndex]) {
                throw std::runtime_error("Failed to create a fullscreen binding set.");
            }
        }
    }

} // namespace lumin::render

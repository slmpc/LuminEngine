#include "render/features/raster/RasterFeatureResources.hpp"

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

    RasterFeatureResources::RasterFeatureResources(nvrhi::IDevice& device, std::uint32_t frameSlotCount)
        : device_(device), resources_(device), frames_(frameSlotCount) {
        if (frameSlotCount == 0) {
            throw std::invalid_argument("Raster Feature resources require at least one frame slot.");
        }
    }

    RasterFeatureResources::~RasterFeatureResources() {
        destroy();
    }

    void RasterFeatureResources::create(std::uint32_t width, std::uint32_t height) {
        destroy();
        if (width == 0 || height == 0) {
            throw std::invalid_argument("Raster Feature resources require a non-zero render extent.");
        }
        try {
            createImages(width, height);
        } catch (...) {
            destroy();
            throw;
        }
    }

    void RasterFeatureResources::destroy() noexcept {
        for (RasterFeatureFrameResources& frameResources : frames_) {
            for (GpuTexture& shadow : frameResources.shadowCascades) {
                resources_.destroyTexture(shadow);
            }
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
        shadowDepthFormat_ = nvrhi::Format::UNKNOWN;
    }

    const RasterFeatureFrameResources& RasterFeatureResources::frame(std::uint32_t frameIndex) const {
        if (frameIndex >= frames_.size()) {
            throw std::out_of_range("Raster Feature frame index is out of range.");
        }
        return frames_[frameIndex];
    }

    std::uint32_t RasterFeatureResources::frameSlotCount() const noexcept {
        return static_cast<std::uint32_t>(frames_.size());
    }

    nvrhi::Format RasterFeatureResources::positionFormat() const noexcept {
        return positionFormat_;
    }

    nvrhi::Format RasterFeatureResources::normalFormat() const noexcept {
        return normalFormat_;
    }

    nvrhi::Format RasterFeatureResources::albedoFormat() const noexcept {
        return albedoFormat_;
    }

    nvrhi::Format RasterFeatureResources::motionFormat() const noexcept {
        return motionFormat_;
    }

    nvrhi::Format RasterFeatureResources::materialIdFormat() const noexcept {
        return materialIdFormat_;
    }

    nvrhi::Format RasterFeatureResources::depthFormat() const noexcept {
        return depthFormat_;
    }

    nvrhi::Format RasterFeatureResources::shadowDepthFormat() const noexcept {
        return shadowDepthFormat_;
    }

    nvrhi::Format RasterFeatureResources::chooseFormat(std::span<const nvrhi::Format> candidates,
                                                       nvrhi::FormatSupport required) const {
        for (const nvrhi::Format candidate : candidates) {
            if (supports(device_.queryFormatSupport(candidate), required)) {
                return candidate;
            }
        }
        throw std::runtime_error("No supported NvRHI format satisfies the Raster Feature usage.");
    }

    GpuTexture RasterFeatureResources::createTexture(const nvrhi::TextureDesc& desc) const {
        GpuTexture result;
        result.texture = device_.createTexture(desc);
        if (!result.texture) {
            throw std::runtime_error("Failed to create a Raster Feature texture.");
        }
        result.format = desc.format;
        result.width = desc.width;
        result.height = desc.height;
        result.mipLevels = desc.mipLevels;
        result.arrayLayers = desc.arraySize;
        result.initialState = desc.initialState;
        return result;
    }

    void RasterFeatureResources::createImages(std::uint32_t width, std::uint32_t height) {
        const nvrhi::FormatSupport colorSampled =
            nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderSample;
        const nvrhi::FormatSupport storageSampled =
            colorSampled | nvrhi::FormatSupport::ShaderUavLoad | nvrhi::FormatSupport::ShaderUavStore;
        const nvrhi::FormatSupport depthAttachment = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::DepthStencil;
        const nvrhi::FormatSupport depthSampled = depthAttachment | nvrhi::FormatSupport::ShaderSample;
        const nvrhi::FormatSupport integerStorage =
            nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderLoad |
            nvrhi::FormatSupport::ShaderUavLoad | nvrhi::FormatSupport::ShaderUavStore;

        constexpr std::array positionCandidates = {nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA32_FLOAT};
        constexpr std::array albedoCandidates = {nvrhi::Format::RGBA8_UNORM, nvrhi::Format::RGBA16_FLOAT,
                                                 nvrhi::Format::RGBA32_FLOAT, nvrhi::Format::BGRA8_UNORM};
        constexpr std::array motionCandidates = {nvrhi::Format::RG16_FLOAT, nvrhi::Format::RGBA16_FLOAT};
        constexpr std::array materialIdCandidates = {nvrhi::Format::R32_UINT};
        constexpr std::array depthCandidates = {nvrhi::Format::D32, nvrhi::Format::D32S8, nvrhi::Format::D24S8};
        constexpr std::array shadowCandidates = {nvrhi::Format::D32, nvrhi::Format::D16};

        positionFormat_ = chooseFormat(positionCandidates, storageSampled);
        normalFormat_ = positionFormat_;
        albedoFormat_ = chooseFormat(albedoCandidates, storageSampled);
        motionFormat_ = chooseFormat(motionCandidates, storageSampled);
        materialIdFormat_ = chooseFormat(materialIdCandidates, integerStorage);
        depthFormat_ = chooseFormat(depthCandidates, depthAttachment);
        shadowDepthFormat_ = chooseFormat(shadowCandidates, depthSampled);

        for (RasterFeatureFrameResources& frameResources : frames_) {
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

            desc = textureDesc(shadowMapResolution, shadowMapResolution, shadowDepthFormat_, "Shadow cascade");
            desc.isRenderTarget = true;
            desc.isShaderResource = true;
            for (GpuTexture& shadow : frameResources.shadowCascades) {
                shadow = createTexture(desc);
            }
        }
    }

} // namespace lumin::render

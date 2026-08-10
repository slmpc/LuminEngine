#include "render/atmosphere/AtmosphereLutResources.hpp"

#include <limits>
#include <stdexcept>

namespace lumin::render::atmosphere {
    namespace {

        constexpr std::uint64_t rgba16FloatBytesPerTexel = 8;

        [[nodiscard]] constexpr bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                                                     std::uint64_t& result) noexcept {
            if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] bool tryPayloadBytes(const AtmosphereLutExtent& extent, std::uint64_t& bytes) noexcept {
            if (extent.width == 0 || extent.height == 0 || extent.depth == 0) {
                return false;
            }

            std::uint64_t texels = 0;
            return checkedMultiply(extent.width, extent.height, texels) &&
                   checkedMultiply(texels, extent.depth, texels) &&
                   checkedMultiply(texels, rgba16FloatBytesPerTexel, bytes);
        }

        [[nodiscard]] bool validPayloadExtent(const AtmosphereLutExtent& extent) noexcept {
            std::uint64_t bytes = 0;
            return tryPayloadBytes(extent, bytes);
        }

        [[nodiscard]] constexpr bool containsFormatSupport(nvrhi::FormatSupport available,
                                                           nvrhi::FormatSupport required) noexcept {
            const auto availableBits = static_cast<std::uint32_t>(available);
            const auto requiredBits = static_cast<std::uint32_t>(required);
            return (availableBits & requiredBits) == requiredBits;
        }

        [[nodiscard]] AtmosphereLutResourceDesc makeResourceDesc(AtmosphereLut lut, nvrhi::TextureDimension dimension,
                                                                 AtmosphereLutExtent extent) {
            return AtmosphereLutResourceDesc{
                .lut = lut,
                .dimension = dimension,
                .extent = extent,
                .format = nvrhi::Format::RGBA16_FLOAT,
                .usage = AtmosphereLutUsage::Sampled | AtmosphereLutUsage::Storage,
                .requiredFormatSupport = requiredAtmosphereLutFormatSupport(),
            };
        }

    } // namespace

    std::size_t atmosphereLutResourceIndex(AtmosphereLut lut) {
        const std::size_t index = static_cast<std::size_t>(lut);
        if (index >= atmosphereLutResourceCount) {
            throw std::invalid_argument("Atmosphere LUT is invalid.");
        }
        return index;
    }

    nvrhi::FormatSupport requiredAtmosphereLutFormatSupport() noexcept {
        return nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderSample |
               nvrhi::FormatSupport::ShaderUavStore;
    }

    bool validateAtmosphereLutQuality(const AtmosphereLutQuality& quality) noexcept {
        if (quality.transmittance.depth != 1 || quality.multiScattering.depth != 1 || quality.skyView.depth != 1) {
            return false;
        }

        const std::array extents{
            quality.transmittance,
            quality.multiScattering,
            quality.skyView,
            quality.aerialPerspective,
        };
        std::uint64_t total = 0;
        for (const AtmosphereLutExtent& extent : extents) {
            std::uint64_t bytes = 0;
            if (!tryPayloadBytes(extent, bytes) || bytes > std::numeric_limits<std::uint64_t>::max() - total) {
                return false;
            }
            total += bytes;
        }
        return true;
    }

    bool validateAtmosphereLutResourceDesc(const AtmosphereLutResourceDesc& desc) noexcept {
        const std::size_t index = static_cast<std::size_t>(desc.lut);
        if (index >= atmosphereLutResourceCount || !validPayloadExtent(desc.extent) ||
            desc.format != nvrhi::Format::RGBA16_FLOAT ||
            !hasAtmosphereLutUsage(desc.usage, AtmosphereLutUsage::Sampled | AtmosphereLutUsage::Storage) ||
            !containsFormatSupport(desc.requiredFormatSupport, requiredAtmosphereLutFormatSupport())) {
            return false;
        }

        if (desc.lut == AtmosphereLut::AerialPerspective) {
            return desc.dimension == nvrhi::TextureDimension::Texture3D;
        }
        return desc.dimension == nvrhi::TextureDimension::Texture2D && desc.extent.depth == 1;
    }

    AtmosphereLutResourceSet makeAtmosphereLutResourceSet(const AtmosphereLutQuality& quality) {
        if (!validateAtmosphereLutQuality(quality)) {
            throw std::invalid_argument("Atmosphere LUT quality is invalid.");
        }

        return AtmosphereLutResourceSet{
            makeResourceDesc(AtmosphereLut::Transmittance, nvrhi::TextureDimension::Texture2D, quality.transmittance),
            makeResourceDesc(AtmosphereLut::MultiScattering, nvrhi::TextureDimension::Texture2D,
                             quality.multiScattering),
            makeResourceDesc(AtmosphereLut::SkyView, nvrhi::TextureDimension::Texture2D, quality.skyView),
            makeResourceDesc(AtmosphereLut::AerialPerspective, nvrhi::TextureDimension::Texture3D,
                             quality.aerialPerspective),
        };
    }

    const AtmosphereLutResourceDesc& atmosphereLutResource(const AtmosphereLutResourceSet& resources,
                                                           AtmosphereLut lut) {
        return resources[atmosphereLutResourceIndex(lut)];
    }

    std::uint64_t estimateAtmosphereLutPayloadBytes(const AtmosphereLutResourceDesc& desc) {
        if (!validateAtmosphereLutResourceDesc(desc)) {
            throw std::invalid_argument("Atmosphere LUT resource description is invalid.");
        }

        std::uint64_t bytes = 0;
        if (!tryPayloadBytes(desc.extent, bytes)) {
            throw std::overflow_error("Atmosphere LUT payload byte count overflows uint64.");
        }
        return bytes;
    }

    std::uint64_t estimateAtmosphereLutPayloadBytes(const AtmosphereLutResourceSet& resources) {
        std::uint64_t total = 0;
        for (const AtmosphereLutResourceDesc& desc : resources) {
            const std::uint64_t bytes = estimateAtmosphereLutPayloadBytes(desc);
            if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
                throw std::overflow_error("Atmosphere LUT payload total overflows uint64.");
            }
            total += bytes;
        }
        return total;
    }

    bool supportsAtmosphereLutFormat(const AtmosphereLutResourceDesc& desc,
                                     nvrhi::FormatSupport availableSupport) noexcept {
        return validateAtmosphereLutResourceDesc(desc) &&
               containsFormatSupport(availableSupport, desc.requiredFormatSupport);
    }

} // namespace lumin::render::atmosphere

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <nvrhi/nvrhi.h>

#include "render/atmosphere/AtmosphereLutScheduler.hpp"

namespace lumin::render::atmosphere {

    /** 可由大气质量预设调整的单个 LUT 范围。 */
    struct AtmosphereLutExtent {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 1;

        friend constexpr bool operator==(const AtmosphereLutExtent&, const AtmosphereLutExtent&) noexcept = default;
    };

    /**
     * 大气 LUT 的质量配置。
     *
     * 二维 LUT 的 `depth` 必须为一；空中透视使用三维纹理。默认值优先保证紧凑渲染器的质量与显存平衡。
     */
    struct AtmosphereLutQuality {
        AtmosphereLutExtent transmittance{256, 64, 1};
        AtmosphereLutExtent multiScattering{32, 32, 1};
        AtmosphereLutExtent skyView{256, 256, 1};
        AtmosphereLutExtent aerialPerspective{32, 32, 32};

        friend constexpr bool operator==(const AtmosphereLutQuality&, const AtmosphereLutQuality&) noexcept = default;
    };

    /** LUT 的逻辑用途；同步状态仍由 `FrameGraph` 根据 pass 声明推导。 */
    enum class AtmosphereLutUsage : std::uint8_t {
        None = 0,
        Sampled = 1U << 0U,
        Storage = 1U << 1U,
    };

    [[nodiscard]] constexpr AtmosphereLutUsage operator|(AtmosphereLutUsage left, AtmosphereLutUsage right) noexcept {
        return static_cast<AtmosphereLutUsage>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    [[nodiscard]] constexpr AtmosphereLutUsage operator&(AtmosphereLutUsage left, AtmosphereLutUsage right) noexcept {
        return static_cast<AtmosphereLutUsage>(static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
    }

    /** 返回用途集合是否完整包含指定用途。 */
    [[nodiscard]] constexpr bool hasAtmosphereLutUsage(AtmosphereLutUsage usages,
                                                       AtmosphereLutUsage required) noexcept {
        return (usages & required) == required;
    }

    /** 单个持久化大气 LUT 的后端无关资源契约。 */
    struct AtmosphereLutResourceDesc {
        AtmosphereLut lut = AtmosphereLut::Transmittance;
        nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Texture2D;
        AtmosphereLutExtent extent;
        nvrhi::Format format = nvrhi::Format::RGBA16_FLOAT;
        AtmosphereLutUsage usage = AtmosphereLutUsage::Sampled | AtmosphereLutUsage::Storage;
        nvrhi::FormatSupport requiredFormatSupport = nvrhi::FormatSupport::None;

        friend constexpr bool operator==(const AtmosphereLutResourceDesc&,
                                         const AtmosphereLutResourceDesc&) noexcept = default;
    };

    inline constexpr std::size_t atmosphereLutResourceCount = static_cast<std::size_t>(AtmosphereLut::Count);
    using AtmosphereLutResourceSet = std::array<AtmosphereLutResourceDesc, atmosphereLutResourceCount>;

    /**
     * 返回 LUT 对应的稳定数组索引。
     *
     * @throws std::invalid_argument `lut` 不是已知 LUT 时抛出。
     */
    [[nodiscard]] std::size_t atmosphereLutResourceIndex(AtmosphereLut lut);

    /** 返回全部 LUT 共同要求的 NvRHI 格式能力。 */
    [[nodiscard]] nvrhi::FormatSupport requiredAtmosphereLutFormatSupport() noexcept;

    /** 返回质量配置是否具有合法维度且其紧密排列 payload 不会溢出 64 位字节数。 */
    [[nodiscard]] bool validateAtmosphereLutQuality(const AtmosphereLutQuality& quality) noexcept;

    /** 返回资源描述是否满足当前大气 LUT 的格式、维度、用途和能力契约。 */
    [[nodiscard]] bool validateAtmosphereLutResourceDesc(const AtmosphereLutResourceDesc& desc) noexcept;

    /**
     * 从质量配置构建四个稳定排序的资源描述。
     *
     * 排序固定为 Transmittance、MultiScattering、SkyView、AerialPerspective。
     *
     * @throws std::invalid_argument 质量配置非法时抛出。
     */
    [[nodiscard]] AtmosphereLutResourceSet makeAtmosphereLutResourceSet(const AtmosphereLutQuality& quality = {});

    /** 返回资源集合中指定 LUT 的描述。 */
    [[nodiscard]] const AtmosphereLutResourceDesc& atmosphereLutResource(const AtmosphereLutResourceSet& resources,
                                                                         AtmosphereLut lut);

    /**
     * 估算单个 LUT 紧密排列的 texel payload 字节数。
     *
     * 该值不包含设备分配对齐、元数据或驱动开销，因此不是 Vulkan 实际 allocation 大小。
     *
     * @throws std::invalid_argument 描述违反大气资源契约时抛出。
     */
    [[nodiscard]] std::uint64_t estimateAtmosphereLutPayloadBytes(const AtmosphereLutResourceDesc& desc);

    /** 估算四个 LUT 的紧密排列 texel payload 总字节数。 */
    [[nodiscard]] std::uint64_t estimateAtmosphereLutPayloadBytes(const AtmosphereLutResourceSet& resources);

    /** 返回设备报告的格式能力是否覆盖该资源的全部要求。 */
    [[nodiscard]] bool supportsAtmosphereLutFormat(const AtmosphereLutResourceDesc& desc,
                                                   nvrhi::FormatSupport availableSupport) noexcept;

} // namespace lumin::render::atmosphere

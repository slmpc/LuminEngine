#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace lumin::render::core {

    /// 枚举渲染 Feature 可声明的硬件或运行时集成能力。
    enum class RenderCapability : std::uint8_t {
        /// 支持图形光栅化管线。
        Graphics,
        /// 支持计算着色器。
        Compute,
        /// 支持动态渲染。
        DynamicRendering,
        /// 支持描述符索引和绑定数组。
        DescriptorIndexing,
        /// 支持缓冲区设备地址。
        BufferDeviceAddress,
        /// 支持加速结构构建与访问。
        AccelerationStructure,
        /// 支持 Ray Tracing Pipeline。
        RayTracingPipeline,
        /// 支持内联 Ray Query。
        RayQuery,
        /// 支持 16 位浮点着色器运算。
        ShaderFloat16,
        /// 支持 64 位整数着色器运算。
        ShaderInt64,
        /// 支持着色器 subgroup 运算。
        SubgroupOperations,
        /// 支持 32 位浮点原子操作。
        AtomicFloat32,
        /// 当前构建包含 NRD 运行时集成。
        Nrd,
        /// 当前构建包含 SHARC 运行时集成。
        Sharc,
        /// 枚举项数量，不表示可声明能力。
        Count
    };

    /**
     * @brief 使用紧凑位集保存一组 `RenderCapability`。
     *
     * 集合是值类型，可直接复制；对 `RenderCapability::Count` 的查询始终返回 `false`。
     */
    class CapabilitySet final {
    public:
        /// 构造空能力集合。
        constexpr CapabilitySet() noexcept = default;

        /// 从能力列表构造集合，重复项会被合并。
        constexpr CapabilitySet(std::initializer_list<RenderCapability> capabilities) noexcept {
            for (const RenderCapability capability : capabilities) {
                add(capability);
            }
        }

        /// 将能力加入集合并返回当前对象。
        constexpr CapabilitySet& add(RenderCapability capability) noexcept {
            bits_ |= bitFor(capability);
            return *this;
        }

        /// 从集合移除能力并返回当前对象。
        constexpr CapabilitySet& remove(RenderCapability capability) noexcept {
            bits_ &= ~bitFor(capability);
            return *this;
        }

        /// 返回集合是否包含指定能力。
        [[nodiscard]] constexpr bool contains(RenderCapability capability) const noexcept {
            const std::uint64_t bit = bitFor(capability);
            return bit != 0 && (bits_ & bit) != 0;
        }

        /// 返回集合是否包含 `required` 中的全部能力。
        [[nodiscard]] constexpr bool containsAll(CapabilitySet required) const noexcept {
            return (bits_ & required.bits_) == required.bits_;
        }

        /// 返回集合是否包含 `candidates` 中的任意能力。
        [[nodiscard]] constexpr bool containsAny(CapabilitySet candidates) const noexcept {
            return (bits_ & candidates.bits_) != 0;
        }

        /// 返回当前集合与 `other` 的交集。
        [[nodiscard]] constexpr CapabilitySet intersection(CapabilitySet other) const noexcept {
            return fromBits(bits_ & other.bits_);
        }

        /// 返回当前集合减去 `other` 后剩余的能力。
        [[nodiscard]] constexpr CapabilitySet difference(CapabilitySet other) const noexcept {
            return fromBits(bits_ & ~other.bits_);
        }

        /// 返回集合中的能力数量。
        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return static_cast<std::size_t>(std::popcount(bits_));
        }

        /// 返回集合是否为空。
        [[nodiscard]] constexpr bool empty() const noexcept {
            return bits_ == 0;
        }

        friend constexpr bool operator==(const CapabilitySet&, const CapabilitySet&) noexcept = default;

        /// 返回两个集合的并集。
        friend constexpr CapabilitySet operator|(CapabilitySet left, CapabilitySet right) noexcept {
            return fromBits(left.bits_ | right.bits_);
        }

        /// 返回两个集合的交集。
        friend constexpr CapabilitySet operator&(CapabilitySet left, CapabilitySet right) noexcept {
            return fromBits(left.bits_ & right.bits_);
        }

    private:
        explicit constexpr CapabilitySet(std::uint64_t bits) noexcept : bits_(bits) {
        }

        [[nodiscard]] static constexpr std::uint64_t bitFor(RenderCapability capability) noexcept {
            const auto index = static_cast<std::uint8_t>(capability);
            return index >= static_cast<std::uint8_t>(RenderCapability::Count) ? 0 : std::uint64_t{1} << index;
        }

        [[nodiscard]] static constexpr CapabilitySet fromBits(std::uint64_t bits) noexcept {
            return CapabilitySet(bits);
        }

        std::uint64_t bits_ = 0;
    };

    /**
     * @brief 保存 Feature 规划阶段可使用的设备能力快照。
     *
     * 该结构只描述后端无关能力与规划上限，不拥有设备或任何 GPU 资源。
     */
    struct RenderDeviceCapabilities {
        /// 设备及当前构建共同提供的能力。
        CapabilitySet supported;

        /// 后端允许同时存在的最大帧槽数量。
        std::uint32_t maxFramesInFlight = 1;

        /// Ray Tracing Pipeline 支持的最大递归深度；不支持时为零。
        std::uint32_t maxRayRecursionDepth = 0;

        /// 返回设备是否支持单项能力。
        [[nodiscard]] constexpr bool supports(RenderCapability capability) const noexcept {
            return supported.contains(capability);
        }

        /// 返回设备是否支持集合中的全部能力。
        [[nodiscard]] constexpr bool supportsAll(CapabilitySet capabilities) const noexcept {
            return supported.containsAll(capabilities);
        }
    };

    /// 返回能力的稳定英文标识，用于日志和错误消息。
    [[nodiscard]] std::string_view renderCapabilityName(RenderCapability capability) noexcept;

} // namespace lumin::render::core

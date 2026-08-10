#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "render/FrameGraph.hpp"
#include "render/atmosphere/AtmosphereLutResources.hpp"

namespace lumin::render::atmosphere {

    /** compute pass 对 LUT 的声明式访问类型。 */
    enum class AtmosphereLutPassAccess : std::uint8_t {
        SampledRead,
        StorageWrite,
    };

    /** 单个 compute pass 的 LUT 访问声明。 */
    struct AtmosphereLutPassResourceUse {
        AtmosphereLut lut = AtmosphereLut::Transmittance;
        AtmosphereLutPassAccess access = AtmosphereLutPassAccess::SampledRead;

        friend constexpr bool operator==(const AtmosphereLutPassResourceUse&,
                                         const AtmosphereLutPassResourceUse&) noexcept = default;
    };

    /**
     * 单个大气 LUT compute pass 的不可变规划描述。
     *
     * `dependencies()` 只列出本次计划内必须先执行的生产者；跨帧已提交 LUT 通过 `resourceUses()` 继续显式读取。
     */
    class AtmosphereLutComputePass final {
    public:
        /** 从目标 LUT、资源访问和计划内生产者依赖构造不可变 pass 描述。 */
        AtmosphereLutComputePass(AtmosphereLut target, std::vector<AtmosphereLutPassResourceUse> resourceUses,
                                 std::vector<AtmosphereLut> dependencies);

        /** 返回该 pass 写入的目标 LUT。 */
        [[nodiscard]] AtmosphereLut target() const noexcept;

        /** 返回稳定调试名称。 */
        [[nodiscard]] std::string_view name() const noexcept;

        /** 返回全部 SRV 读取与 UAV 写入声明。 */
        [[nodiscard]] std::span<const AtmosphereLutPassResourceUse> resourceUses() const noexcept;

        /** 返回当前计划内的直接生产者依赖。 */
        [[nodiscard]] std::span<const AtmosphereLut> dependencies() const noexcept;

    private:
        AtmosphereLut target_ = AtmosphereLut::Transmittance;
        std::vector<AtmosphereLutPassResourceUse> resourceUses_;
        std::vector<AtmosphereLut> dependencies_;
    };

    /** 大气 LUT pass 的同步所有权；本层没有任何手写 barrier 描述。 */
    enum class AtmosphereLutSynchronizationOwner : std::uint8_t {
        FrameGraph,
    };

    /** 从 scheduler 的重建集合派生出的不可变 compute pass 计划。 */
    class AtmosphereLutPassPlan final {
    public:
        /** 大气 pass 永远由 `FrameGraph` 根据资源访问声明生成同步。 */
        static constexpr AtmosphereLutSynchronizationOwner synchronizationOwner =
            AtmosphereLutSynchronizationOwner::FrameGraph;

        /** 返回计划对应的逻辑帧序号。 */
        [[nodiscard]] core::RenderSequence sequence() const noexcept;

        /** 返回按稳定拓扑排序的 compute pass。 */
        [[nodiscard]] std::span<const AtmosphereLutComputePass> passes() const noexcept;

        /** 返回计划是否不需要录制任何大气 compute pass。 */
        [[nodiscard]] bool empty() const noexcept;

        /** 返回计划是否由有效 scheduler 计划构建。 */
        [[nodiscard]] bool isValid() const noexcept;

    private:
        friend AtmosphereLutPassPlan makeAtmosphereLutPassPlan(const AtmosphereLutPlan& lutPlan);

        core::RenderSequence sequence_;
        std::vector<AtmosphereLutComputePass> passes_;
    };

    /**
     * 将 LUT 重建集合转换为最小 compute pass 集。
     *
     * 调用不会提交 scheduler 事务，也不会推进任何 LUT generation。
     *
     * @throws std::invalid_argument `lutPlan` 无效时抛出。
     */
    [[nodiscard]] AtmosphereLutPassPlan makeAtmosphereLutPassPlan(const AtmosphereLutPlan& lutPlan);

    /** 保存四个持久化 LUT 与当前帧槽常量缓冲在 `FrameGraph` 中的资源句柄。 */
    struct AtmosphereLutFrameGraphResources {
        std::array<FrameGraphResourceHandle, atmosphereLutResourceCount> textures{};
        FrameGraphResourceHandle constants;

        /** 返回指定 LUT 的纹理句柄。 */
        [[nodiscard]] FrameGraphResourceHandle texture(AtmosphereLut lut) const;

        /** 返回四张纹理与常量缓冲句柄是否都有效。 */
        [[nodiscard]] bool isValid() const noexcept;
    };

    /** 保存每类 LUT 的 compute dispatch 回调；回调不得自行录制资源 barrier。 */
    struct AtmosphereLutExecuteCallbacks {
        std::array<FrameGraph::ExecuteCallback, atmosphereLutResourceCount> callbacks{};
    };

    /** 一次注册返回的 pass 句柄；未重建的 LUT 保持无效句柄。 */
    struct AtmosphereLutFrameGraphPasses {
        std::array<FrameGraphPassHandle, atmosphereLutResourceCount> handles{};

        /** 返回指定 LUT 的 pass 句柄。 */
        [[nodiscard]] FrameGraphPassHandle pass(AtmosphereLut lut) const;

        /** 返回本次实际注册的 pass 数。 */
        [[nodiscard]] std::size_t size() const noexcept;
    };

    /**
     * 把规划结果注册为现有 `FrameGraph` 的 compute pass。
     *
     * setup 仅声明常量缓冲读取、`ShaderResource` 读取、`UnorderedAccess` 写入和 pass 拓扑依赖；所有 barrier
     * 由 `FrameGraph` 统一生成。execute 回调只应录制对应 compute dispatch。
     *
     * @throws std::invalid_argument 计划或资源句柄无效时抛出。
     */
    [[nodiscard]] AtmosphereLutFrameGraphPasses
    registerAtmosphereLutPasses(FrameGraph& frameGraph, const AtmosphereLutPassPlan& plan,
                                const AtmosphereLutFrameGraphResources& resources,
                                AtmosphereLutExecuteCallbacks callbacks = {});

} // namespace lumin::render::atmosphere

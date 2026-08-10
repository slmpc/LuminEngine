#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "render/atmosphere/AtmosphereTypes.hpp"

namespace lumin::render::atmosphere {

    /** 可独立持久化与重建的大气查找表。 */
    enum class AtmosphereLut : std::uint8_t {
        Transmittance,
        MultiScattering,
        SkyView,
        AerialPerspective,
        Count,
    };

    /** 一个 LUT 在最近一次成功提交后的已提交状态。 */
    struct AtmosphereLutState {
        bool valid = false;
        std::uint64_t generation = 0;
        core::RenderSequence lastRebuildSequence;

        friend constexpr bool operator==(const AtmosphereLutState&, const AtmosphereLutState&) noexcept = default;
    };

    /** 一次大气 LUT 录制尝试的输入。 */
    struct AtmosphereLutFrameInput {
        core::RenderSequence sequence;
        AtmosphereLutSignatures signatures;

        /**
         * 强制重建全部 LUT。
         *
         * 外部纹理生命周期失效（例如关闭大气后重新启用或重建渲染资源）时应设为 `true`。若本次录制被放弃，
         * 调用方必须在后续重试中继续设置该标志，直到重建帧成功提交。
         */
        bool forceRebuild = false;
    };

    /**
     * 一次录制尝试的不可变 LUT 重建计划。
     *
     * 计划只描述提交后的目标代数；读取计划不会推进调度器状态。
     */
    class AtmosphereLutPlan final {
    public:
        AtmosphereLutPlan() noexcept = default;

        /** 返回计划对应的逻辑帧序号。 */
        [[nodiscard]] core::RenderSequence sequence() const noexcept;

        /** 返回指定 LUT 是否必须在本帧重建。 */
        [[nodiscard]] bool rebuilds(AtmosphereLut lut) const;

        /** 返回成功提交后指定 LUT 应具有的代数。 */
        [[nodiscard]] std::uint64_t generationAfterSubmit(AtmosphereLut lut) const;

        /** 返回计划是否由调度器完整初始化。 */
        [[nodiscard]] bool isValid() const noexcept;

    private:
        friend class AtmosphereLutScheduler;

        static constexpr std::size_t lutCount = static_cast<std::size_t>(AtmosphereLut::Count);

        AtmosphereLutPlan(core::RenderSequence sequence, std::array<bool, lutCount> rebuild,
                          std::array<std::uint64_t, lutCount> generations) noexcept;

        core::RenderSequence sequence_;
        std::array<bool, lutCount> rebuild_{};
        std::array<std::uint64_t, lutCount> generations_{};
    };

    /**
     * 根据四类输入签名生成最小 LUT 重建集合，并以成功提交为事务边界。
     *
     * `beginFrame()` 只建立活动事务；只有 `commitSubmittedFrame()` 会提交签名并推进对应 LUT 的代数。
     * 失败帧必须调用 `abandonFrame()`，同一逻辑序号随后可以重试。
     */
    class AtmosphereLutScheduler final {
    public:
        /** 为一次录制尝试生成最小重建计划；`forceRebuild` 会覆盖签名复用并重建全部 LUT。 */
        [[nodiscard]] AtmosphereLutPlan beginFrame(const AtmosphereLutFrameInput& input);

        /** 在 GPU 提交成功后原子提交活动计划。 */
        void commitSubmittedFrame(core::RenderSequence sequence);

        /** 放弃失败的录制尝试，不推进任何签名或 LUT 代数。 */
        void abandonFrame(core::RenderSequence sequence);

        /** 返回当前是否存在尚未结束的录制事务。 */
        [[nodiscard]] bool hasActiveFrame() const noexcept;

        /** 返回指定 LUT 的最近已提交状态。 */
        [[nodiscard]] const AtmosphereLutState& state(AtmosphereLut lut) const;

        /** 返回最近一次成功提交的逻辑序号。 */
        [[nodiscard]] core::RenderSequence lastSuccessfulSequence() const noexcept;

        /** 返回已提交签名；首个成功提交前返回 `nullptr`。 */
        [[nodiscard]] const AtmosphereLutSignatures* committedSignatures() const noexcept;

    private:
        static constexpr std::size_t lutCount = static_cast<std::size_t>(AtmosphereLut::Count);

        void requireActiveSequence(core::RenderSequence sequence) const;

        std::array<AtmosphereLutState, lutCount> states_{};
        std::optional<AtmosphereLutSignatures> committedSignatures_;
        std::optional<AtmosphereLutFrameInput> activeInput_;
        std::optional<AtmosphereLutPlan> activePlan_;
        core::RenderSequence lastSuccessfulSequence_;
    };

} // namespace lumin::render::atmosphere

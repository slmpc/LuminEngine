#pragma once

#include "render/core/FrameIdentity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace lumin::render::world {

    enum class SceneChangeMask : std::uint8_t;

} // namespace lumin::render::world

namespace lumin::render::core {

    /// 描述可能影响跨帧历史有效性的变化原因位掩码。
    enum class HistoryReason : std::uint32_t {
        /// 未发生影响历史的变化。
        None = 0,
        /// 当前是渲染序列的第一帧。
        FirstFrame = 1U << 0U,
        /// 相机发生切镜或不可连续的姿态跳变。
        CameraCut = 1U << 1U,
        /// 内部渲染范围发生变化。
        RenderExtentChanged = 1U << 2U,
        /// 交换链已重建。
        SwapchainRecreated = 1U << 3U,
        /// 场景对象、网格或实例拓扑发生变化。
        SceneTopologyChanged = 1U << 4U,
        /// 场景内容发生连续但显著的变化。
        SceneContentChanged = 1U << 5U,
        /// 光源或曝光等照明参数发生变化。
        LightingChanged = 1U << 6U,
        /// 渲染 Feature 的启用状态或关键配置发生变化。
        FeatureConfigurationChanged = 1U << 7U,
        /// 相关 Shader 已重新加载。
        ShaderReloaded = 1U << 8U,
        /// 大气散射参数或太阳大气参数发生变化。
        AtmosphereParametersChanged = 1U << 9U,
        /// 世界坐标原点发生重定位。
        WorldOriginRebased = 1U << 10U,
        /// 设备丢失后已恢复并重新创建资源。
        DeviceRecovered = 1U << 11U,
        /// 当前版本定义的全部变化原因。
        All = (1U << 12U) - 1U
    };

    /// 返回两个历史原因掩码的并集。
    [[nodiscard]] constexpr HistoryReason operator|(HistoryReason left, HistoryReason right) noexcept {
        return static_cast<HistoryReason>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
    }

    /// 返回两个历史原因掩码的交集。
    [[nodiscard]] constexpr HistoryReason operator&(HistoryReason left, HistoryReason right) noexcept {
        return static_cast<HistoryReason>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
    }

    /// 返回两个历史原因掩码的异或结果。
    [[nodiscard]] constexpr HistoryReason operator^(HistoryReason left, HistoryReason right) noexcept {
        return static_cast<HistoryReason>(static_cast<std::uint32_t>(left) ^ static_cast<std::uint32_t>(right));
    }

    /// 返回当前版本已知原因范围内的按位取反结果。
    [[nodiscard]] constexpr HistoryReason operator~(HistoryReason reason) noexcept {
        return static_cast<HistoryReason>(~static_cast<std::uint32_t>(reason) &
                                          static_cast<std::uint32_t>(HistoryReason::All));
    }

    /// 将 `right` 原因合并到 `left`。
    constexpr HistoryReason& operator|=(HistoryReason& left, HistoryReason right) noexcept {
        left = left | right;
        return left;
    }

    /// 仅保留 `left` 与 `right` 共有的原因。
    constexpr HistoryReason& operator&=(HistoryReason& left, HistoryReason right) noexcept {
        left = left & right;
        return left;
    }

    /// 枚举彼此独立维护的跨帧历史域。
    enum class HistoryDomain : std::uint8_t {
        /// Temporal Anti-Aliasing 历史。
        Taa,
        /// NRD diffuse 去噪历史。
        NrdDiffuse,
        /// NRD specular 去噪历史。
        NrdSpecular,
        /// SHARC 世界空间辐照度缓存。
        Sharc,
        /// 大气系统预计算查找表。
        AtmosphereLut,
        /// 枚举项数量，不表示有效历史域。
        Count
    };

    /**
     * @brief 指示历史域在本帧开始前应执行的动作。
     *
     * 枚举值按破坏性递增，可通过 `strongerHistoryAction()` 合并多个原因。
     */
    enum class HistoryAction : std::uint8_t {
        /// 保留全部历史内容。
        Keep,
        /// 降低历史权重或局部刷新，但保留可复用内容。
        SoftReset,
        /// 丢弃全部历史并从当前帧重新建立。
        FullReset
    };

    /// 返回两个历史动作中破坏性更强的一个。
    [[nodiscard]] constexpr HistoryAction strongerHistoryAction(HistoryAction left, HistoryAction right) noexcept {
        return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right) ? left : right;
    }

    /**
     * @brief 汇总一次逻辑帧相对上一帧发生的历史相关变化。
     *
     * 该值可由窗口、场景、Feature 配置和设备恢复路径分别累加，随后一次性提交给历史策略。
     */
    class FrameChangeSet final {
    public:
        /// 构造空变化集合。
        constexpr FrameChangeSet() noexcept = default;

        /// 从一个或多个原因构造变化集合。
        explicit constexpr FrameChangeSet(HistoryReason reasons) noexcept : reasons_(reasons) {
        }

        /// 加入一个或多个变化原因。
        constexpr FrameChangeSet& add(HistoryReason reasons) noexcept {
            reasons_ |= reasons;
            return *this;
        }

        /// 移除一个或多个变化原因。
        constexpr FrameChangeSet& remove(HistoryReason reasons) noexcept {
            reasons_ &= ~reasons;
            return *this;
        }

        /// 合并另一个变化集合。
        constexpr FrameChangeSet& merge(const FrameChangeSet& other) noexcept {
            return add(other.reasons_);
        }

        /// 清空全部变化原因。
        constexpr void clear() noexcept {
            reasons_ = HistoryReason::None;
        }

        /// 返回是否包含 `reasons` 中的任意原因。
        [[nodiscard]] constexpr bool containsAny(HistoryReason reasons) const noexcept {
            return (reasons_ & reasons) != HistoryReason::None;
        }

        /// 返回是否包含 `reasons` 中的全部原因。
        [[nodiscard]] constexpr bool containsAll(HistoryReason reasons) const noexcept {
            return (reasons_ & reasons) == reasons;
        }

        /// 返回集合是否为空。
        [[nodiscard]] constexpr bool empty() const noexcept {
            return reasons_ == HistoryReason::None;
        }

        /// 返回完整原因位掩码。
        [[nodiscard]] constexpr HistoryReason reasons() const noexcept {
            return reasons_;
        }

    private:
        HistoryReason reasons_ = HistoryReason::None;
    };

    /**
     * @brief 将每个历史域和变化原因映射为对应动作。
     *
     * 默认构造的所有映射均为 `Keep`。使用 `makeDefaultHistoryPolicyMap()` 获取引擎标准策略。
     */
    class HistoryPolicyMap final {
    public:
        /// 构造全部映射均为 `Keep` 的策略表。
        HistoryPolicyMap() noexcept;

        /**
         * @brief 为历史域的一项或多项原因设置动作。
         * @throws std::invalid_argument `domain`、`action` 无效，或原因为空、包含未知位时抛出。
         */
        void setPolicy(HistoryDomain domain, HistoryReason reasons, HistoryAction action);

        /**
         * @brief 查询历史域对单一原因的直接映射。
         * @throws std::invalid_argument `domain` 无效或 `reason` 不是单一已知原因时抛出。
         */
        [[nodiscard]] HistoryAction policy(HistoryDomain domain, HistoryReason reason) const;

        /**
         * @brief 计算变化集合对指定历史域要求的最强动作。
         * @throws std::invalid_argument `domain` 无效或变化集合包含未知原因位时抛出。
         */
        [[nodiscard]] HistoryAction actionFor(HistoryDomain domain, const FrameChangeSet& changes) const;

    private:
        static constexpr std::size_t domainCount = static_cast<std::size_t>(HistoryDomain::Count);
        static constexpr std::size_t reasonCount = 12;

        std::array<std::array<HistoryAction, reasonCount>, domainCount> policies_{};
    };

    /**
     * @brief 创建 TAA、NRD、SHARC 与大气 LUT 的标准历史失效策略。
     *
     * 屏幕空间历史在切镜或范围变化时完全重置；SHARC 保留与视图无关的世界空间缓存；
     * 大气 LUT 仅在其输入、Shader、Feature 配置或设备资源变化时重建。
     */
    [[nodiscard]] HistoryPolicyMap makeDefaultHistoryPolicyMap();

    /**
     * @brief 将渲染世界增量转换为跨帧历史变化。
     *
     * 几何、实例拓扑和材质绑定变化会破坏跨帧对应关系，映射为 `SceneTopologyChanged`；
     * 实例变换或材质参数变化映射为 `SceneContentChanged`。照明与大气参数保持独立原因，
     * 以便 SHARC、NRD 和大气 LUT 分别选择自己的失效强度。
     *
     * @throws std::invalid_argument `sceneChanges` 包含未知变化位时抛出。
     */
    [[nodiscard]] FrameChangeSet frameChangesFromScene(world::SceneChangeMask sceneChanges);

    /**
     * @brief 描述准备录制的一帧所观察到的历史输入。
     *
     * `cameraCutEpoch` 必须来自活动相机的显式切镜代数，普通连续相机移动不得修改它。
     * `changes` 可合并场景增量、交换链重建、Shader 热重载等一次性事件。
     */
    struct HistoryFrameObservation {
        /// 本次录制尝试的逻辑帧序号。
        RenderSequence sequence;

        /// 当前活动相机的显式切镜代数。
        std::uint64_t cameraCutEpoch = 0;

        /// 当前内部渲染范围。
        RenderExtent renderExtent;

        /// 除相机切镜和范围变化以外，由调用方汇总的变化。
        FrameChangeSet changes;
    };

    /**
     * @brief 一次录制尝试的只读分域历史计划。
     *
     * Feature 在录制命令前读取自己的动作。计划本身不推进任何历史状态；只有对应帧成功提交后，
     * `HistoryCoordinator::commitSubmittedFrame()` 才会提交计划。
     */
    class HistoryFramePlan final {
    public:
        /// 构造无效的空计划。
        HistoryFramePlan() noexcept = default;

        /// 返回该计划对应的逻辑帧序号。
        [[nodiscard]] RenderSequence sequence() const noexcept;

        /// 返回本次计划合并后的全部变化原因。
        [[nodiscard]] const FrameChangeSet& changes() const noexcept;

        /**
         * @brief 返回指定历史域在录制前应执行的动作。
         * @throws std::invalid_argument `domain` 不是有效历史域时抛出。
         */
        [[nodiscard]] HistoryAction actionFor(HistoryDomain domain) const;

        /// 返回计划是否由协调器完整初始化。
        [[nodiscard]] bool isValid() const noexcept;

    private:
        friend class HistoryCoordinator;

        static constexpr std::size_t domainCount = static_cast<std::size_t>(HistoryDomain::Count);

        HistoryFramePlan(RenderSequence sequence, FrameChangeSet changes,
                         std::array<HistoryAction, domainCount> actions) noexcept;

        RenderSequence sequence_;
        FrameChangeSet changes_;
        std::array<HistoryAction, domainCount> actions_{};
    };

    /**
     * @brief 记录一个历史域已经由 GPU 接受的进度。
     *
     * 该状态只在成功提交后更新。录制失败、提交失败或主动放弃帧时，所有字段均保持不变。
     */
    struct HistoryDomainState {
        /// 至少有一帧已成功建立该域的历史。
        bool valid = false;

        /// 已成功提交的完全重置次数，可用于选择或重建域内资源代数。
        std::uint64_t resetEpoch = 0;

        /// 已被该域接受的成功提交帧数。
        std::uint64_t acceptedFrameCount = 0;

        /// 最近一次成功提交所使用的动作。
        HistoryAction lastCommittedAction = HistoryAction::Keep;

        /// 最近一次成功提交的逻辑帧序号；`valid == false` 时无效。
        RenderSequence lastSuccessfulSequence;

        friend constexpr bool operator==(const HistoryDomainState&, const HistoryDomainState&) noexcept = default;
    };

    /**
     * @brief 统一协调 TAA、NRD、SHARC 和大气 LUT 的跨帧状态。
     *
     * 协调器采用两阶段协议：`beginFrame()` 只生成录制计划，`commitSubmittedFrame()` 在 GPU 提交成功后
     * 原子推进全部历史域及相机/范围基线。若本帧未提交，调用 `abandonFrame()`；一次性变化会被保留并在
     * 下一次 `beginFrame()` 中重放，避免失败路径错误消费 camera cut、场景增量或重建事件。
     */
    class HistoryCoordinator final {
    public:
        /// 使用引擎标准分域策略构造协调器。
        HistoryCoordinator();

        /// 使用调用方提供的策略构造协调器。
        explicit HistoryCoordinator(HistoryPolicyMap policies);

        /**
         * @brief 开始一次帧录制并返回不可变动作计划。
         *
         * 相机切镜与范围变化相对于最近一次成功提交的观察值计算，而不是相对于最近一次录制尝试计算。
         * 首次成功提交前，每次尝试都会自动带有 `FirstFrame`。
         *
         * @throws std::invalid_argument 序号无效、范围为空、序号未前进或变化包含未知位时抛出。
         * @throws std::logic_error 已有尚未提交或放弃的帧时抛出。
         */
        [[nodiscard]] HistoryFramePlan beginFrame(const HistoryFrameObservation& observation);

        /**
         * @brief 在对应 GPU 提交成功后提交当前计划。
         *
         * 只有该函数会推进历史有效性、重置代数、成功帧计数以及相机/范围比较基线。
         *
         * @throws std::logic_error 当前没有活动计划或 `sequence` 与活动计划不匹配时抛出。
         */
        void commitSubmittedFrame(RenderSequence sequence);

        /**
         * @brief 放弃未成功提交的当前计划，并保留其变化供下一次尝试使用。
         * @throws std::logic_error 当前没有活动计划或 `sequence` 与活动计划不匹配时抛出。
         */
        void abandonFrame(RenderSequence sequence);

        /// 返回当前是否存在尚未结束的录制计划。
        [[nodiscard]] bool hasActiveFrame() const noexcept;

        /**
         * @brief 返回指定历史域最近一次成功提交后的状态。
         * @throws std::invalid_argument `domain` 不是有效历史域时抛出。
         */
        [[nodiscard]] const HistoryDomainState& state(HistoryDomain domain) const;

        /// 返回最近一次成功提交的逻辑帧序号；首次成功提交前无效。
        [[nodiscard]] RenderSequence lastSuccessfulSequence() const noexcept;

        /// 返回尚待成功提交消费的一次性变化。
        [[nodiscard]] const FrameChangeSet& pendingChanges() const noexcept;

    private:
        static constexpr std::size_t domainCount = static_cast<std::size_t>(HistoryDomain::Count);

        void requireActiveSequence(RenderSequence sequence) const;

        HistoryPolicyMap policies_;
        std::array<HistoryDomainState, domainCount> states_{};
        FrameChangeSet pendingChanges_;
        std::optional<HistoryFramePlan> activePlan_;
        std::optional<HistoryFrameObservation> activeObservation_;
        RenderSequence lastSuccessfulSequence_;
        std::uint64_t committedCameraCutEpoch_ = 0;
        RenderExtent committedRenderExtent_{};
    };

} // namespace lumin::render::core

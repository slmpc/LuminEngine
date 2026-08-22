#pragma once

#include "render/core/FrameDataContract.hpp"
#include "render/core/History.hpp"
#include "render/core/RenderCapabilities.hpp"

#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lumin::render::core {

    /**
     * @brief Feature 的稳定拥有型标识。
     *
     * 标识在计划内部按值保存，因此不会依赖外部字符串生命周期。空字符串会被构造函数拒绝。
     */
    class FeatureId final {
    public:
        /// 复制 `value` 并构造稳定标识；空字符串会抛出 `std::invalid_argument`。
        explicit FeatureId(std::string_view value);

        /// 返回标识字符串，其生命周期与当前 `FeatureId` 相同。
        [[nodiscard]] const std::string& value() const noexcept;

        friend auto operator<=>(const FeatureId&, const FeatureId&) noexcept = default;

    private:
        std::string value_;
    };

    /// 为 `FeatureId` 提供字符串内容哈希，可用于标准无序容器，但不得将哈希值持久化。
    struct FeatureIdHash final {
        /// 返回 Feature 标识字符串的哈希值。
        [[nodiscard]] std::size_t operator()(const FeatureId& id) const noexcept;
    };

    /// 定义 Feature 的必需能力或依赖不可用时采用的策略。
    enum class MissingRequirementPolicy {
        /// 禁用当前 Feature，并继续生成其余可用 Feature 的计划。
        DisableFeature,
        /// 拒绝整个计划并抛出 `std::runtime_error`。
        RejectPlan
    };

    /**
     * @brief 声明一个可规划渲染 Feature 的静态契约。
     *
     * `dependencies` 是强依赖：只有全部依赖已启用，当前 Feature 才能启用。
     * `optionalCapabilities` 不影响启用结果，解析后仅报告实际可用的交集。
     */
    struct FeatureDescriptor {
        /// 使用稳定标识构造描述符，其余字段采用安全默认值。
        explicit FeatureDescriptor(FeatureId featureId);

        /// Feature 的唯一稳定标识。
        FeatureId id;

        /// Feature 启用所需的全部能力。
        CapabilitySet requiredCapabilities;

        /// Feature 可利用但不强制要求的能力。
        CapabilitySet optionalCapabilities;

        /// 必须先启用并先执行规划的 Feature。
        std::vector<FeatureId> dependencies;

        /// 当前 Feature 构建帧图前必须已经发布的数据契约。
        std::vector<FrameDataContract> requiredInputs;

        /// 当前 Feature 可以使用、但不会影响激活结果的数据契约。
        std::vector<FrameDataContract> optionalInputs;

        /// 当前 Feature 向后续 Feature 发布的数据契约。
        std::vector<FrameDataContract> outputs;

        /// 不通过数据流表达的显式顺序约束；应只用于外部副作用。
        std::vector<FeatureId> after;

        /// 必需能力或强依赖不可用时的处理策略。
        MissingRequirementPolicy missingRequirementPolicy = MissingRequirementPolicy::DisableFeature;

        /// 当前 Feature 写入并负责维护的时序历史域；同一计划内每个域只能由一个 Feature 拥有。
        std::vector<HistoryDomain> historyDomains;
    };

    /// 描述 Feature 经过设备能力与依赖解析后的状态。
    enum class FeatureActivation {
        /// Feature 已启用并会出现在执行顺序中。
        Enabled,
        /// Feature 因缺少必需能力而被禁用。
        DisabledMissingCapabilities,
        /// Feature 因强依赖未启用而被禁用。
        DisabledDependency
    };

    /// 保存单个 Feature 的解析结果。
    struct ResolvedRenderFeature {
        /// 使用 Feature 标识构造默认启用的解析结果。
        explicit ResolvedRenderFeature(FeatureId featureId);

        /// Feature 的稳定标识。
        FeatureId id;

        /// 最终启用状态。
        FeatureActivation activation = FeatureActivation::Enabled;

        /// 设备实际提供的可选能力交集。
        CapabilitySet availableOptionalCapabilities;

        /// 导致 Feature 禁用的缺失必需能力；非能力原因禁用时为空。
        CapabilitySet missingRequiredCapabilities;

        /// 导致 Feature 禁用的首个不可用强依赖；非依赖原因禁用时为空。
        std::optional<FeatureId> unavailableDependency;

        /// 当前 Feature 负责推进和失效处理的历史域。
        std::vector<HistoryDomain> historyDomains;

        /// 返回 Feature 是否已启用。
        [[nodiscard]] bool enabled() const noexcept;
    };

    /**
     * @brief 保存一次 Feature 解析的不可变结果视图。
     *
     * `features()` 包含全部 Feature，并按拓扑顺序排列；`executionOrder()` 只包含已启用 Feature。
     */
    class ResolvedRenderFeaturePlan final {
    public:
        /// 返回按拓扑顺序排列的全部解析结果。
        [[nodiscard]] std::span<const ResolvedRenderFeature> features() const noexcept;

        /// 返回只包含已启用 Feature 的稳定执行顺序。
        [[nodiscard]] std::span<const FeatureId> executionOrder() const noexcept;

        /// 按标识查找解析结果；计划中不存在该标识时返回 `nullptr`。
        [[nodiscard]] const ResolvedRenderFeature* find(const FeatureId& id) const noexcept;

    private:
        friend class RenderFeaturePlan;

        std::vector<ResolvedRenderFeature> features_;
        std::vector<FeatureId> executionOrder_;
    };

    /**
     * @brief 注册、验证并解析渲染 Feature 依赖图。
     *
     * 注册顺序用于打破多个合法拓扑顺序之间的平局，使生成结果在相同输入下保持确定。
     * 该类只负责静态规划，不创建 Feature 实例或 GPU 资源。
     */
    class RenderFeaturePlan final {
    public:
        /// 注册 Feature；结构错误会在 `validate()`、`topologicalOrder()` 或 `resolve()` 时报告。
        void addFeature(FeatureDescriptor descriptor);

        /// 清空全部已注册 Feature。
        void clear() noexcept;

        /// 返回按注册顺序保存的描述符。
        [[nodiscard]] std::span<const FeatureDescriptor> descriptors() const noexcept;

        /**
         * @brief 验证标识唯一性、能力集合、依赖引用、历史域唯一所有权以及依赖环。
         * @throws std::invalid_argument 描述符存在结构错误时抛出。
         */
        void validate() const;

        /**
         * @brief 返回全部 Feature 的确定性拓扑顺序。
         * @throws std::invalid_argument 计划结构无效时抛出。
         */
        [[nodiscard]] std::vector<FeatureId> topologicalOrder() const;

        /**
         * @brief 根据设备能力和缺失策略生成可执行计划。
         * @throws std::invalid_argument 计划结构无效时抛出。
         * @throws std::runtime_error `RejectPlan` Feature 的必需条件不可用时抛出。
         */
        [[nodiscard]] ResolvedRenderFeaturePlan resolve(const RenderDeviceCapabilities& deviceCapabilities) const;

    private:
        std::vector<FeatureDescriptor> descriptors_;
    };

} // namespace lumin::render::core

#pragma once

#include "render/core/History.hpp"
#include "render/core/RenderFeaturePlan.hpp"

#include <any>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace lumin::render::core {

    /// 描述 Feature 设置变化对渲染状态造成的可组合影响。
    enum class SettingsChangeImpact : std::uint8_t {
        /// 设置值没有发生有效变化。
        None = 0,
        /// 可在下一帧直接读取新值，无需重置其他状态。
        HotUpdate = 1U << 0U,
        /// 必须按 `historyReasons` 失效相关历史域。
        HistoryReset = 1U << 1U,
        /// 必须在安全帧边界重新解析并替换 PipelineInstance。
        PipelineRecompose = 1U << 2U,
        /// 当前 Feature 必须重新创建持久 GPU 资源。
        ResourceRecreate = 1U << 3U,
    };

    /// 返回两个设置影响掩码的并集。
    [[nodiscard]] constexpr SettingsChangeImpact operator|(SettingsChangeImpact left,
                                                           SettingsChangeImpact right) noexcept {
        return static_cast<SettingsChangeImpact>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /// 将 `right` 中的设置影响合并到 `left`。
    constexpr SettingsChangeImpact& operator|=(SettingsChangeImpact& left, SettingsChangeImpact right) noexcept {
        left = left | right;
        return left;
    }

    /// 返回 `value` 是否包含 `mask` 中任意设置影响。
    [[nodiscard]] constexpr bool hasAnyImpact(SettingsChangeImpact value, SettingsChangeImpact mask) noexcept {
        return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(mask)) != 0;
    }

    /// 保存一次或多次 Feature 设置变化合并后的影响。
    struct FeatureSettingsChange {
        /// 需要执行的全部设置更新动作。
        SettingsChangeImpact impact = SettingsChangeImpact::None;
        /// `HistoryReset` 使用的具体历史失效原因。
        FrameChangeSet historyReasons;

        /// 合并另一项影响并返回当前对象。
        FeatureSettingsChange& merge(const FeatureSettingsChange& other) noexcept;
    };

    /**
     * @brief 按值拥有一组 Feature 设置的不可变共享快照。
     *
     * 快照可跨线程传递；构造后其内部映射不会再修改。`get<T>()` 返回的引用只在快照生命周期内有效。
     */
    class RenderSettingsSnapshot final {
    public:
        /// 构造不包含任何 Feature 设置的空快照。
        RenderSettingsSnapshot();

        /// 返回快照是否包含指定 Feature 的设置。
        [[nodiscard]] bool contains(const FeatureId& id) const noexcept;

        /**
         * @brief 返回指定 Feature 的类型化设置。
         * @throws std::out_of_range 快照中不存在该 Feature 时抛出。
         * @throws std::invalid_argument `T` 与注册 schema 类型不一致时抛出。
         */
        template <typename T> [[nodiscard]] const T& get(const FeatureId& id) const {
            const auto found = values_->find(id);
            if (found == values_->end()) {
                throw std::out_of_range("Render settings snapshot does not contain Feature: " + id.value());
            }
            const T* value = std::any_cast<T>(&found->second);
            if (value == nullptr) {
                throw std::invalid_argument("Render settings type does not match Feature schema: " + id.value());
            }
            return *value;
        }

    private:
        friend class RenderSettingsStore;
        friend class RenderSettingsSchemaRegistry;
        using Values = std::unordered_map<FeatureId, std::any, FeatureIdHash>;

        explicit RenderSettingsSnapshot(std::shared_ptr<const Values> values);

        std::shared_ptr<const Values> values_;
    };

    /** 保存 Feature 设置类型、默认值、校验器和变更分类器。 */
    class RenderSettingsSchemaRegistry final {
    public:
        /**
         * @brief 注册一个 Feature 的设置类型、默认值、校验器和可选变更分类器。
         * @throws std::invalid_argument Feature 已注册，或默认值未通过校验时抛出。
         */
        template <typename T>
        void registerSchema(FeatureId id, T defaults, std::function<void(const T&)> validator = {},
                            std::function<FeatureSettingsChange(const T&, const T&)> differ = {}) {
            if (schemas_.contains(id)) {
                throw std::invalid_argument("Duplicate Render settings schema: " + id.value());
            }
            if (validator) {
                validator(defaults);
            }

            Schema schema;
            schema.type = std::type_index(typeid(T));
            schema.defaults = std::move(defaults);
            schema.validate = [validator = std::move(validator)](const std::any& value) {
                const T* typed = std::any_cast<T>(&value);
                if (typed == nullptr) {
                    throw std::invalid_argument("Render settings value has an incompatible type.");
                }
                if (validator) {
                    validator(*typed);
                }
            };
            schema.diff = [differ = std::move(differ)](const std::any& before, const std::any& after) {
                const T& typedBefore = std::any_cast<const T&>(before);
                const T& typedAfter = std::any_cast<const T&>(after);
                if (differ) {
                    return differ(typedBefore, typedAfter);
                }
                if constexpr (std::equality_comparable<T>) {
                    return typedBefore == typedAfter
                               ? FeatureSettingsChange{}
                               : FeatureSettingsChange{.impact = SettingsChangeImpact::HotUpdate, .historyReasons = {}};
                }
                return FeatureSettingsChange{.impact = SettingsChangeImpact::HotUpdate, .historyReasons = {}};
            };
            schemas_.emplace(std::move(id), std::move(schema));
        }

        /// 返回指定 Feature 是否已有设置 schema。
        [[nodiscard]] bool contains(const FeatureId& id) const noexcept;

        /// 返回已注册 schema 数量。
        [[nodiscard]] std::size_t size() const noexcept;

        /**
         * @brief 比较两个完整快照并合并全部 Feature 的变化影响。
         * @throws std::invalid_argument 快照与当前 schema 集合不一致或值类型错误时抛出。
         */
        [[nodiscard]] FeatureSettingsChange diff(const RenderSettingsSnapshot& before,
                                                 const RenderSettingsSnapshot& after) const;

    private:
        friend class RenderSettingsStore;

        struct Schema {
            std::type_index type{typeid(void)};
            std::any defaults;
            std::function<void(const std::any&)> validate;
            std::function<FeatureSettingsChange(const std::any&, const std::any&)> diff;
        };

        std::unordered_map<FeatureId, Schema, FeatureIdHash> schemas_;
    };

    /**
     * @brief 主线程可修改的类型化设置集合。
     *
     * Store 不提供内部同步，必须由主线程独占修改；提交渲染前调用 `snapshot()` 生成完全拥有数据的不可变副本。
     * `schemas` 必须覆盖 Store 生命周期，且 Store 构造后不得再改变 registry。
     */
    class RenderSettingsStore final {
    public:
        /// 从全部 schema 的默认值构造设置集合。
        explicit RenderSettingsStore(const RenderSettingsSchemaRegistry& schemas);

        /**
         * @brief 校验并替换指定 Feature 的设置。
         * @throws std::out_of_range Feature 没有注册 schema 时抛出。
         * @throws std::invalid_argument 值类型错误或未通过校验时抛出。
         */
        template <typename T> void set(const FeatureId& id, T value) {
            const auto schema = schemas_->schemas_.find(id);
            if (schema == schemas_->schemas_.end()) {
                throw std::out_of_range("Render settings schema is not registered: " + id.value());
            }
            std::any erased = std::move(value);
            schema->second.validate(erased);
            values_.insert_or_assign(id, std::move(erased));
        }

        /**
         * @brief 返回 Store 中指定 Feature 的类型化设置。
         * @throws std::out_of_range Feature 不存在时抛出。
         * @throws std::invalid_argument `T` 与 schema 类型不一致时抛出。
         */
        template <typename T> [[nodiscard]] const T& get(const FeatureId& id) const {
            const auto found = values_.find(id);
            if (found == values_.end()) {
                throw std::out_of_range("Render settings do not contain Feature: " + id.value());
            }
            const T* value = std::any_cast<T>(&found->second);
            if (value == nullptr) {
                throw std::invalid_argument("Render settings type does not match Feature schema: " + id.value());
            }
            return *value;
        }

        /// 深拷贝当前设置并返回可跨线程传递的不可变快照。
        [[nodiscard]] RenderSettingsSnapshot snapshot() const;

    private:
        const RenderSettingsSchemaRegistry* schemas_ = nullptr;
        RenderSettingsSnapshot::Values values_;
    };

} // namespace lumin::render::core

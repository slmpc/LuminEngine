#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace lumin::render::core {

    /**
     * @brief 按 C++ 具体类型保存本帧渲染数据的类型安全黑板。
     *
     * 每个无 cv/ref 限定类型最多保存一个值；需要保存同一底层类型的多个语义值时，应定义不同包装类型。
     * `set<T>()` 和 `emplace<T>()` 对已存在类型采用覆盖语义，并使旧值的引用立即失效。
     * 黑板支持 move-only 值，自身也仅可移动，通常应限定在一帧的构建和执行阶段使用。
     * 该容器不提供内部同步；并发访问必须由调用方保证互斥。
     */
    class RenderBlackboard final {
    public:
        /// 构造空黑板。
        RenderBlackboard() = default;

        /// 销毁黑板及其中保存的全部值。
        ~RenderBlackboard() = default;

        /// 黑板不允许复制，以避免隐式复制大型帧数据。
        RenderBlackboard(const RenderBlackboard&) = delete;

        /// 黑板不允许复制赋值。
        RenderBlackboard& operator=(const RenderBlackboard&) = delete;

        /// 移动构造黑板并转移全部值的所有权。
        RenderBlackboard(RenderBlackboard&&) noexcept = default;

        /// 移动赋值黑板并转移全部值的所有权。
        RenderBlackboard& operator=(RenderBlackboard&&) noexcept = default;

        /**
         * @brief 保存值并按其无 cv/ref 限定类型建立索引。
         *
         * 若该类型已存在，会先成功构造新值再替换旧值，从而保留构造异常时的原值。
         * @return 新保存值的引用。
         */
        template <typename T> std::remove_cvref_t<T>& set(T&& value) {
            using Value = std::remove_cvref_t<T>;
            return emplace<Value>(std::forward<T>(value));
        }

        /**
         * @brief 原地构造并保存指定类型。
         *
         * 若该类型已存在，会先成功构造新值再替换旧值；旧值引用在替换后失效。
         * @return 新保存值的引用。
         */
        template <typename T, typename... Arguments> T& emplace(Arguments&&... arguments) {
            validateType<T>();
            auto entry = std::make_unique<Entry<T>>(std::forward<Arguments>(arguments)...);
            T& value = entry->value;
            entries_.insert_or_assign(std::type_index(typeid(T)), std::move(entry));
            return value;
        }

        /// 返回黑板是否保存了指定的无 cv/ref 限定类型。
        template <typename T> [[nodiscard]] bool contains() const noexcept {
            validateType<T>();
            return entries_.contains(std::type_index(typeid(T)));
        }

        /**
         * @brief 返回指定类型的可修改引用。
         * @throws std::out_of_range 黑板不包含该类型时抛出。
         */
        template <typename T> [[nodiscard]] T& get() {
            T* value = tryGet<T>();
            if (value == nullptr) {
                throw std::out_of_range("RenderBlackboard does not contain the requested type.");
            }
            return *value;
        }

        /**
         * @brief 返回指定类型的只读引用。
         * @throws std::out_of_range 黑板不包含该类型时抛出。
         */
        template <typename T> [[nodiscard]] const T& get() const {
            const T* value = tryGet<T>();
            if (value == nullptr) {
                throw std::out_of_range("RenderBlackboard does not contain the requested type.");
            }
            return *value;
        }

        /// 返回指定类型的可修改指针；黑板不包含该类型时返回 `nullptr`。
        template <typename T> [[nodiscard]] T* tryGet() noexcept {
            validateType<T>();
            const auto iterator = entries_.find(std::type_index(typeid(T)));
            if (iterator == entries_.end()) {
                return nullptr;
            }
            return &static_cast<Entry<T>*>(iterator->second.get())->value;
        }

        /// 返回指定类型的只读指针；黑板不包含该类型时返回 `nullptr`。
        template <typename T> [[nodiscard]] const T* tryGet() const noexcept {
            validateType<T>();
            const auto iterator = entries_.find(std::type_index(typeid(T)));
            if (iterator == entries_.end()) {
                return nullptr;
            }
            return &static_cast<const Entry<T>*>(iterator->second.get())->value;
        }

        /// 移除指定类型并返回该类型此前是否存在。
        template <typename T> bool erase() noexcept {
            validateType<T>();
            return entries_.erase(std::type_index(typeid(T))) != 0;
        }

        /// 移除全部值；此前取得的所有引用和指针立即失效。
        void clear() noexcept {
            entries_.clear();
        }

        /// 返回当前保存的不同类型数量。
        [[nodiscard]] std::size_t size() const noexcept {
            return entries_.size();
        }

        /// 返回黑板是否为空。
        [[nodiscard]] bool empty() const noexcept {
            return entries_.empty();
        }

    private:
        class EntryBase {
        public:
            virtual ~EntryBase() = default;
        };

        template <typename T> class Entry final : public EntryBase {
        public:
            template <typename... Arguments>
            explicit Entry(Arguments&&... arguments) : value(std::forward<Arguments>(arguments)...) {
            }

            T value;
        };

        template <typename T> static constexpr void validateType() noexcept {
            static_assert(std::is_object_v<T>, "RenderBlackboard values must be object types.");
            static_assert(!std::is_array_v<T>, "RenderBlackboard values must not be array types.");
            static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                          "RenderBlackboard type queries must use an unqualified value type.");
        }

        std::unordered_map<std::type_index, std::unique_ptr<EntryBase>> entries_;
    };

} // namespace lumin::render::core

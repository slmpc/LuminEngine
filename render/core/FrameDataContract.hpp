#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>

namespace lumin::render::core {

    /**
     * @brief 标识 Feature 在一帧内交换的一种强类型数据。
     *
     * 契约相等性只由移除 cv/ref 后的 C++ 类型决定，诊断名仅用于错误信息。需要表达不同业务语义时必须定义不同
     * 包装类型，不能为同一底层类型注册多个名称。
     */
    class FrameDataContract final {
    public:
        /**
         * @brief 为对象类型创建拥有诊断名的契约。
         * @throws std::invalid_argument `diagnosticName` 为空时抛出。
         */
        template <typename T> [[nodiscard]] static FrameDataContract of(std::string_view diagnosticName) {
            using Value = std::remove_cvref_t<T>;
            static_assert(std::is_object_v<Value> && !std::is_array_v<Value>);
            return FrameDataContract{std::type_index(typeid(Value)), diagnosticName};
        }

        /// 返回契约对应的无 cv/ref C++ 类型索引。
        [[nodiscard]] const std::type_index& type() const noexcept;

        /// 返回契约拥有的诊断名。
        [[nodiscard]] const std::string& name() const noexcept;

        /// 按 C++ 类型比较两个数据契约；诊断名不参与比较。
        friend bool operator==(const FrameDataContract& left, const FrameDataContract& right) noexcept {
            return left.type_ == right.type_;
        }

    private:
        FrameDataContract(std::type_index type, std::string_view diagnosticName);

        std::type_index type_;
        std::string name_;
    };

    /// 为 `FrameDataContract` 提供基于 C++ 类型的进程内哈希。
    struct FrameDataContractHash final {
        /// 返回契约类型的哈希；结果不得跨进程或持久化使用。
        [[nodiscard]] std::size_t operator()(const FrameDataContract& contract) const noexcept;
    };

} // namespace lumin::render::core

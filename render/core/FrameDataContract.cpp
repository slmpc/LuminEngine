#include "render/core/FrameDataContract.hpp"

#include <stdexcept>

namespace lumin::render::core {

    FrameDataContract::FrameDataContract(std::type_index type, std::string_view diagnosticName)
        : type_(type), name_(diagnosticName) {
        if (name_.empty()) {
            throw std::invalid_argument("Frame data contracts require a diagnostic name.");
        }
    }

    const std::type_index& FrameDataContract::type() const noexcept {
        return type_;
    }

    const std::string& FrameDataContract::name() const noexcept {
        return name_;
    }

    std::size_t FrameDataContractHash::operator()(const FrameDataContract& contract) const noexcept {
        return contract.type().hash_code();
    }

} // namespace lumin::render::core

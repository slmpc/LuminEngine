#pragma once

#include <filesystem>
#include <memory>

#include "lumin/render/gi/GlobalIllumination.hpp"

namespace lumin::render::gi {

    [[nodiscard]] std::unique_ptr<GlobalIlluminationBackend> makeSsaoBackend(std::filesystem::path shaderDirectory);

}

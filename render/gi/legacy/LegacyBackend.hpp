#pragma once

#include <memory>

#include "render/gi/GlobalIllumination.hpp"

namespace lumin::render::gi {

    /** 创建在 `CreateInfo` 中获取 session 级 shader 缓存的 raster AO backend。 */
    [[nodiscard]] std::unique_ptr<GlobalIlluminationBackend> makeLegacyBackend();

} // namespace lumin::render::gi

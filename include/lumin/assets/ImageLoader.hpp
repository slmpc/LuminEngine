#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace lumin::assets {

    struct ImageData {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;

        [[nodiscard]] bool empty() const noexcept;
    };

    class ImageLoader {
    public:
        [[nodiscard]] static ImageData load(const std::filesystem::path& path);
    };

} // namespace lumin::assets

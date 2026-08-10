#include "assets/ImageLoader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace lumin::assets {
    namespace {

        struct StbiImageDeleter {
            void operator()(stbi_uc* pixels) const noexcept {
                stbi_image_free(pixels);
            }
        };

        using StbiImage = std::unique_ptr<stbi_uc, StbiImageDeleter>;

        [[nodiscard]] std::runtime_error makeImageError(const std::string& path, const std::string& reason) {
            return std::runtime_error("Failed to load image '" + path + "': " + reason);
        }

        [[nodiscard]] std::string stbiFailureReason() {
            const char* reason = stbi_failure_reason();
            return reason != nullptr ? reason : "unknown stb_image error";
        }

        std::string pathForMessage(const std::filesystem::path& path) {
            const std::u8string utf8 = path.generic_u8string();
            return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        std::vector<std::uint8_t> readEncodedImage(const std::filesystem::path& path, const std::string& pathString) {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) {
                throw makeImageError(pathString, "could not open file");
            }

            const std::streamoff encodedSize = stream.tellg();
            if (encodedSize <= 0) {
                throw makeImageError(pathString, "encoded image is empty");
            }
            if (static_cast<std::uintmax_t>(encodedSize) >
                static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
                throw makeImageError(pathString, "encoded image exceeds stb_image's input-size limit");
            }

            std::vector<std::uint8_t> encoded(static_cast<std::size_t>(encodedSize));
            stream.seekg(0, std::ios::beg);
            stream.read(reinterpret_cast<char*>(encoded.data()), encodedSize);
            if (!stream) {
                throw makeImageError(pathString, "could not read the complete file");
            }
            return encoded;
        }

    } // namespace

    bool ImageData::empty() const noexcept {
        return width == 0 || height == 0 || pixels.empty();
    }

    ImageData ImageLoader::load(const std::filesystem::path& path) {
        const std::string pathString = pathForMessage(path);
        const std::vector<std::uint8_t> encoded = readEncodedImage(path, pathString);

        int decodedWidth = 0;
        int decodedHeight = 0;
        StbiImage decoded(stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &decodedWidth,
                                                &decodedHeight, nullptr, STBI_rgb_alpha));

        if (!decoded) {
            throw makeImageError(pathString, stbiFailureReason());
        }

        if (decodedWidth <= 0 || decodedHeight <= 0) {
            throw makeImageError(pathString, "decoded image has invalid dimensions");
        }

        const std::uint64_t width = static_cast<std::uint64_t>(decodedWidth);
        const std::uint64_t height = static_cast<std::uint64_t>(decodedHeight);
        constexpr std::uint64_t channelCount = 4;

        if (width > std::numeric_limits<std::uint32_t>::max() || height > std::numeric_limits<std::uint32_t>::max()) {
            throw makeImageError(pathString, "decoded image dimensions exceed the supported range");
        }

        if (height != 0 && width > std::numeric_limits<std::uint64_t>::max() / height) {
            throw makeImageError(pathString, "decoded image dimensions overflow the pixel count");
        }

        const std::uint64_t pixelCount = width * height;
        if (pixelCount > std::numeric_limits<std::uint64_t>::max() / channelCount) {
            throw makeImageError(pathString, "decoded image dimensions overflow the byte count");
        }

        const std::uint64_t byteCount64 = pixelCount * channelCount;
        std::vector<std::uint8_t> pixels;
        if (byteCount64 > std::numeric_limits<std::size_t>::max() || byteCount64 > pixels.max_size()) {
            throw makeImageError(pathString, "decoded image is too large to fit in memory");
        }

        const std::size_t byteCount = static_cast<std::size_t>(byteCount64);
        pixels.resize(byteCount);
        std::memcpy(pixels.data(), decoded.get(), byteCount);

        ImageData image;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.pixels = std::move(pixels);
        return image;
    }

} // namespace lumin::assets

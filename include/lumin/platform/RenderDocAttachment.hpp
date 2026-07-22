#pragma once

#include <filesystem>

namespace lumin::platform {

    class RenderDocAttachment {
    public:
        RenderDocAttachment() = default;
        explicit RenderDocAttachment(const std::filesystem::path& libraryPath);
        ~RenderDocAttachment();

        RenderDocAttachment(const RenderDocAttachment&) = delete;
        RenderDocAttachment& operator=(const RenderDocAttachment&) = delete;
        RenderDocAttachment(RenderDocAttachment&& other) noexcept;
        RenderDocAttachment& operator=(RenderDocAttachment&&) = delete;

        [[nodiscard]] bool attached() const noexcept;

    private:
        void* library_ = nullptr;
    };

} // namespace lumin::platform

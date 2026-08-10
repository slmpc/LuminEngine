#include "render/platform/RenderDocAttachment.hpp"

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace lumin::platform {

    RenderDocAttachment::RenderDocAttachment(const std::filesystem::path& libraryPath) {
        if (libraryPath.empty()) {
            throw std::invalid_argument("RenderDoc library path cannot be empty");
        }

#if defined(_WIN32)
        HMODULE module = LoadLibraryW(libraryPath.c_str());
        if (module == nullptr) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Failed to load RenderDoc library '" + libraryPath.string() + "'");
        }

        if (GetProcAddress(module, "RENDERDOC_GetAPI") == nullptr) {
            FreeLibrary(module);
            throw std::runtime_error("Library does not export RENDERDOC_GetAPI: " + libraryPath.string());
        }
        library_ = module;
#else
        void* module = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (module == nullptr) {
            throw std::runtime_error("Failed to load RenderDoc library '" + libraryPath.string() + "': " + dlerror());
        }

        if (dlsym(module, "RENDERDOC_GetAPI") == nullptr) {
            dlclose(module);
            throw std::runtime_error("Library does not export RENDERDOC_GetAPI: " + libraryPath.string());
        }
        library_ = module;
#endif
    }

    RenderDocAttachment::~RenderDocAttachment() {
#if defined(_WIN32)
        if (library_ != nullptr) {
            FreeLibrary(static_cast<HMODULE>(library_));
        }
#else
        if (library_ != nullptr) {
            dlclose(library_);
        }
#endif
    }

    RenderDocAttachment::RenderDocAttachment(RenderDocAttachment&& other) noexcept
        : library_(std::exchange(other.library_, nullptr)) {
    }

    bool RenderDocAttachment::attached() const noexcept {
        return library_ != nullptr;
    }

} // namespace lumin::platform

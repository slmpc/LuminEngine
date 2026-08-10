#include "platform/Window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace lumin::platform {

    int Window::windowCount_ = 0;

    Window::Window(const WindowDesc& desc) {
        initializeSdl();

        window_ = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window_ == nullptr) {
            const std::string error = SDL_GetError();
            terminateSdl();
            throw std::runtime_error("Failed to create SDL window: " + error);
        }

        ++windowCount_;
    }

    Window::~Window() {
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            --windowCount_;
        }

        terminateSdl();
    }

    void Window::pollEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            processEvent(event);
        }
    }

    void Window::waitEvents() {
        SDL_Event event;
        if (SDL_WaitEvent(&event)) {
            processEvent(event);
        }

        while (SDL_PollEvent(&event)) {
            processEvent(event);
        }
    }

    void Window::setEventCallback(EventCallback callback) {
        eventCallback_ = std::move(callback);
    }

    bool Window::shouldClose() const {
        return shouldClose_;
    }

    bool Window::framebufferResized() const noexcept {
        return framebufferResized_;
    }

    VkExtent2D Window::framebufferExtent() const {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
            throw std::runtime_error("Failed to query SDL window pixel size: " + std::string(SDL_GetError()));
        }

        VkExtent2D extent;
        extent.width = static_cast<std::uint32_t>(width > 0 ? width : 1);
        extent.height = static_cast<std::uint32_t>(height > 0 ? height : 1);
        return extent;
    }

    std::vector<const char*> Window::requiredInstanceExtensions() const {
        std::uint32_t extensionCount = 0;
        const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        if (extensions == nullptr || extensionCount == 0) {
            throw std::runtime_error("SDL did not provide Vulkan instance extensions: " + std::string(SDL_GetError()));
        }

        return {extensions, extensions + extensionCount};
    }

    VkSurfaceKHR Window::createSurface(VkInstance instance) const {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
            throw std::runtime_error("Failed to create Vulkan window surface: " + std::string(SDL_GetError()));
        }

        return surface;
    }

    SDL_Window* Window::nativeHandle() const noexcept {
        return window_;
    }

    bool Window::isKeyDown(Key key) const noexcept {
        const bool* keyboard = SDL_GetKeyboardState(nullptr);
        SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
        switch (key) {
        case Key::W:
            scancode = SDL_SCANCODE_W;
            break;
        case Key::A:
            scancode = SDL_SCANCODE_A;
            break;
        case Key::S:
            scancode = SDL_SCANCODE_S;
            break;
        case Key::D:
            scancode = SDL_SCANCODE_D;
            break;
        case Key::Space:
            scancode = SDL_SCANCODE_SPACE;
            break;
        case Key::LeftControl:
            scancode = SDL_SCANCODE_LCTRL;
            break;
        case Key::Escape:
            scancode = SDL_SCANCODE_ESCAPE;
            break;
        }
        return scancode != SDL_SCANCODE_UNKNOWN && keyboard[scancode];
    }

    void Window::resetFramebufferResized() noexcept {
        framebufferResized_ = false;
    }

    void Window::processEvent(const SDL_Event& event) {
        if (eventCallback_) {
            eventCallback_(event);
        }

        if (event.type == SDL_EVENT_QUIT) {
            shouldClose_ = true;
            return;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_)) {
            shouldClose_ = true;
        }

        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == SDL_GetWindowID(window_)) {
            framebufferResized_ = true;
        }
    }

    void Window::initializeSdl() {
        if (windowCount_ == 0) {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                throw std::runtime_error("Failed to initialize SDL: " + std::string(SDL_GetError()));
            }
        }
    }

    void Window::terminateSdl() {
        if (windowCount_ == 0) {
            SDL_Quit();
        }
    }

} // namespace lumin::platform

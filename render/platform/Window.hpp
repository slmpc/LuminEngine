#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct SDL_Window;
union SDL_Event;

namespace lumin::platform {

    enum class Key {
        W,
        A,
        S,
        D,
        Space,
        LeftControl,
        Escape,
    };

    enum class MouseButton {
        Right,
    };

    struct MouseDelta {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct WindowDesc {
        int width = 1280;
        int height = 720;
        std::string title = "Lumin Engine";
    };

    class Window {
    public:
        using EventCallback = std::function<void(const SDL_Event&)>;

        explicit Window(const WindowDesc& desc);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void pollEvents();
        void waitEvents();
        void setEventCallback(EventCallback callback);

        [[nodiscard]] bool shouldClose() const;
        [[nodiscard]] bool framebufferResized() const noexcept;
        [[nodiscard]] VkExtent2D framebufferExtent() const;
        [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const;
        [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;
        [[nodiscard]] SDL_Window* nativeHandle() const noexcept;
        [[nodiscard]] bool isKeyDown(Key key) const noexcept;
        [[nodiscard]] bool isMouseButtonDown(MouseButton button) const noexcept;
        [[nodiscard]] MouseDelta mouseDelta() const noexcept;
        /** relative mode 会隐藏鼠标并把移动限制在当前窗口。 */
        void setRelativeMouseMode(bool enabled);
        [[nodiscard]] bool relativeMouseMode() const noexcept;

        void resetFramebufferResized() noexcept;

    private:
        void processEvent(const SDL_Event& event);
        static void initializeSdl();
        static void terminateSdl();

        SDL_Window* window_ = nullptr;
        EventCallback eventCallback_;
        bool shouldClose_ = false;
        bool framebufferResized_ = false;
        bool relativeMouseMode_ = false;
        MouseDelta mouseDelta_;
        static int windowCount_;
    };

} // namespace lumin::platform

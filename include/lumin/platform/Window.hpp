#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace lumin::platform {

    struct WindowDesc {
        int width = 1280;
        int height = 720;
        std::string title = "Lumin Engine";
    };

    class Window {
    public:
        explicit Window(const WindowDesc& desc);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void pollEvents() const;
        void waitEvents() const;

        [[nodiscard]] bool shouldClose() const;
        [[nodiscard]] bool framebufferResized() const noexcept;
        [[nodiscard]] VkExtent2D framebufferExtent() const;
        [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const;
        [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;
        [[nodiscard]] GLFWwindow* nativeHandle() const noexcept;

        void resetFramebufferResized() noexcept;

    private:
        static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
        static void initializeGlfw();
        static void terminateGlfw();

        GLFWwindow* window_ = nullptr;
        bool framebufferResized_ = false;
        static int windowCount_;
    };

} // namespace lumin::platform

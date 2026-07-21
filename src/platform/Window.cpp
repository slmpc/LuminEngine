#include "lumin/platform/Window.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stdexcept>

namespace lumin::platform {

    int Window::windowCount_ = 0;

    Window::Window(const WindowDesc& desc) {
        initializeGlfw();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        window_ = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
        if (window_ == nullptr) {
            terminateGlfw();
            throw std::runtime_error("Failed to create GLFW window.");
        }

        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);

        ++windowCount_;
    }

    Window::~Window() {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
            --windowCount_;
        }

        terminateGlfw();
    }

    void Window::pollEvents() const {
        glfwPollEvents();
    }

    void Window::waitEvents() const {
        glfwWaitEvents();
    }

    bool Window::shouldClose() const {
        return glfwWindowShouldClose(window_) == GLFW_TRUE;
    }

    bool Window::framebufferResized() const noexcept {
        return framebufferResized_;
    }

    VkExtent2D Window::framebufferExtent() const {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);

        VkExtent2D extent;
        extent.width = static_cast<std::uint32_t>(width > 0 ? width : 1);
        extent.height = static_cast<std::uint32_t>(height > 0 ? height : 1);
        return extent;
    }

    std::vector<const char*> Window::requiredInstanceExtensions() const {
        std::uint32_t extensionCount = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (extensions == nullptr || extensionCount == 0) {
            throw std::runtime_error("GLFW did not provide Vulkan instance extensions.");
        }

        return {extensions, extensions + extensionCount};
    }

    VkSurfaceKHR Window::createSurface(VkInstance instance) const {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const VkResult result = glfwCreateWindowSurface(instance, window_, nullptr, &surface);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan window surface.");
        }

        return surface;
    }

    GLFWwindow* Window::nativeHandle() const noexcept {
        return window_;
    }

    void Window::resetFramebufferResized() noexcept {
        framebufferResized_ = false;
    }

    void Window::framebufferResizeCallback(GLFWwindow* window, int, int) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self != nullptr) {
            self->framebufferResized_ = true;
        }
    }

    void Window::initializeGlfw() {
        if (windowCount_ == 0) {
            if (glfwInit() != GLFW_TRUE) {
                throw std::runtime_error("Failed to initialize GLFW.");
            }
        }
    }

    void Window::terminateGlfw() {
        if (windowCount_ == 0) {
            glfwTerminate();
        }
    }

} // namespace lumin::platform

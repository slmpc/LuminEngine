#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <mutex>
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
        Left,
        Middle,
        Right,
    };

    struct FileDialogFilter {
        std::string name;
        std::string pattern;
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
        using FileDialogCallback = std::function<void(std::vector<std::filesystem::path>)>;

        explicit Window(const WindowDesc& desc);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void pollEvents();
        void waitEvents();
        void setEventCallback(EventCallback callback);
        void showOpenFileDialog(std::vector<FileDialogFilter> filters, bool allowMany,
                                FileDialogCallback callback);
        void showOpenFolderDialog(FileDialogCallback callback);
        /** SDL 对话框回调使用；结果将在下一次 pollEvents 后于主线程分发。 */
        void queueDialogResult(FileDialogCallback callback, std::vector<std::filesystem::path> paths);

        [[nodiscard]] bool shouldClose() const;
        void cancelCloseRequest() noexcept;
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
        void dispatchDialogResults();
        static void initializeSdl();
        static void terminateSdl();

        SDL_Window* window_ = nullptr;
        EventCallback eventCallback_;
        bool shouldClose_ = false;
        bool framebufferResized_ = false;
        bool relativeMouseMode_ = false;
        MouseDelta mouseDelta_;
        struct DialogResult {
            FileDialogCallback callback;
            std::vector<std::filesystem::path> paths;
        };
        std::mutex dialogMutex_;
        std::vector<DialogResult> dialogResults_;
        static int windowCount_;
    };

} // namespace lumin::platform

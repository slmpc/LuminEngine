#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "lumin/render/ImGuiContent.hpp"
#include "lumin/render/ImGuiLayer.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::render {
    class VulkanContext;
}

namespace lumin::render {

    class ImGuiManager {
    public:
        ImGuiManager() = default;
        ~ImGuiManager();

        ImGuiManager(const ImGuiManager&) = delete;
        ImGuiManager& operator=(const ImGuiManager&) = delete;

        void initialize(platform::Window& window, VulkanContext& context);
        void shutdown();
        void beginFrame(ImGuiContent* content = nullptr);
        void record(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D extent);
        [[nodiscard]] ImGuiCaptureState captureState() const noexcept;

    private:
        ImGuiLayer layer_;
    };

} // namespace lumin::render

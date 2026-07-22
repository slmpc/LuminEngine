#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "lumin/render/ImGuiLayer.hpp"
#include "lumin/render/RenderSettings.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::render {
    class VulkanContext;
}

namespace lumin::scene {
    class Camera;
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
        void beginFrame();
        void drawLevelPanel(scene::Camera& camera, RenderSettings& settings, std::uint32_t modelCount,
                            std::uint32_t mdiDrawCount);
        void record(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D extent);

    private:
        ImGuiLayer layer_;
    };

}

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <vulkan/vulkan.h>

#include "lumin/render/FrameGraph.hpp"
#include "lumin/render/ImGuiManager.hpp"
#include "lumin/render/ModelRenderer.hpp"
#include "lumin/render/PipelineManager.hpp"
#include "lumin/render/RenderSettings.hpp"
#include "lumin/render/TextureManager.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::scene {
    class Camera;
    class Level;
}

namespace lumin::render {

    class VulkanContext;

    class LevelRenderer {
    public:
        LevelRenderer(platform::Window& window, VulkanContext& context, const scene::Level& level,
                      std::filesystem::path shaderDirectory);
        ~LevelRenderer();

        LevelRenderer(const LevelRenderer&) = delete;
        LevelRenderer& operator=(const LevelRenderer&) = delete;

        void drawFrame(scene::Camera& camera, RenderSettings& settings);
        void waitIdle() const;

        [[nodiscard]] std::uint32_t modelCount() const noexcept;
        [[nodiscard]] std::uint32_t mdiDrawCount() const noexcept;

    private:
        void createRenderResources();
        void destroyRenderResources() noexcept;
        void refreshSwapchainResources();
        void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                 std::uint32_t imageIndex, const scene::Camera& camera);
        void recordGBufferPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                               const scene::Camera& camera);
        void recordPostprocessPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                                   std::uint32_t imageIndex);

        platform::Window& window_;
        VulkanContext& context_;
        const scene::Level& level_;
        std::filesystem::path shaderDirectory_;
        TextureManager textures_;
        PipelineManager pipelines_;
        ImGuiManager imgui_;
        FrameGraph frameGraph_;
        std::unique_ptr<ModelRenderer> modelRenderer_;
        std::uint64_t swapchainGeneration_ = 0;
    };

}

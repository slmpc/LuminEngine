#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>

#include "lumin/render/FrameGraph.hpp"
#include "lumin/render/ImGuiManager.hpp"
#include "lumin/render/ModelRenderer.hpp"
#include "lumin/render/PipelineManager.hpp"
#include "lumin/render/RenderSettings.hpp"
#include "lumin/render/TextureManager.hpp"
#include "lumin/render/gi/GlobalIllumination.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::scene {
    class Camera;
    class Level;
} // namespace lumin::scene

namespace lumin::render {

    class VulkanContext;

    class LevelRenderer {
    public:
        LevelRenderer(platform::Window& window, VulkanContext& context, const scene::Level& level,
                      std::filesystem::path shaderDirectory,
                      std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination = {});
        ~LevelRenderer();

        LevelRenderer(const LevelRenderer&) = delete;
        LevelRenderer& operator=(const LevelRenderer&) = delete;

        void drawFrame(scene::Camera& camera, RenderSettings& settings, ImGuiContent* content = nullptr);
        void waitIdle() const;

        [[nodiscard]] std::uint32_t modelCount() const noexcept;
        [[nodiscard]] std::uint32_t mdiDrawCount() const noexcept;
        [[nodiscard]] gi::BackendInfo globalIlluminationBackendInfo() const noexcept;
        [[nodiscard]] ImGuiCaptureState imguiCaptureState() const noexcept;

    private:
        void createRenderResources();
        void createModelRenderer();
        void destroyRenderResources() noexcept;
        void refreshSwapchainResources();
        void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, std::uint32_t imageIndex,
                                 const scene::Camera& camera, const RenderSettings& settings);
        void recordShadowPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, std::uint32_t cascadeIndex,
                              const glm::mat4& lightViewProjection);
        void recordGBufferPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, const glm::mat4& viewProjection,
                               const glm::mat4& previousViewProjection);
        void recordSkyPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex);
        void recordDeferredLightingPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex);
        void recordTaaPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex);
        void recordHistoryCopy(VkCommandBuffer commandBuffer, std::uint32_t frameIndex);
        void recordTonemapPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, std::uint32_t imageIndex);

        platform::Window& window_;
        VulkanContext& context_;
        const scene::Level& level_;
        std::filesystem::path shaderDirectory_;
        TextureManager textures_;
        PipelineManager pipelines_;
        std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination_;
        ImGuiManager imgui_;
        FrameGraph frameGraph_;
        std::unique_ptr<ModelRenderer> modelRenderer_;
        std::uint64_t swapchainGeneration_ = 0;
        std::uint64_t topologyRevision_ = 0;
        glm::mat4 previousViewProjection_{1.0f};
        glm::vec3 previousCameraPosition_{0.0f};
        glm::vec3 previousCameraForward_{0.0f, 0.0f, -1.0f};
        float previousFieldOfView_ = 0.0f;
        bool hasPreviousCamera_ = false;
        bool previousTaaEnabled_ = true;
        bool previousGlobalIlluminationEnabled_ = true;
        std::uint64_t frameNumber_ = 0;
    };

} // namespace lumin::render

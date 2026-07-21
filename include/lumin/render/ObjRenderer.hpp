#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vulkan/vulkan.h>

#include "lumin/assets/ObjLoader.hpp"
#include "lumin/render/FrameGraph.hpp"
#include "lumin/render/ImGuiLayer.hpp"
#include "lumin/render/PipelineFactory.hpp"
#include "lumin/render/ShaderLibrary.hpp"
#include "lumin/render/VulkanResources.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::render {

    class VulkanContext;

    struct RenderSettings {
        glm::vec3 cameraPosition{0.0f, 1.25f, 3.5f};
        glm::vec3 lightPosition{2.5f, 3.5f, 2.0f};
        glm::vec3 lightColor{1.0f, 0.95f, 0.85f};
        glm::vec3 materialColor{0.82f, 0.68f, 0.48f};
        float ambientStrength = 0.12f;
        float specularStrength = 0.55f;
        float shininess = 48.0f;
        bool smoothShading = true;
        bool showDemoWindow = false;
    };

    class ObjRenderer {
    public:
        ObjRenderer(platform::Window& window, VulkanContext& context, const assets::Mesh& mesh,
                    std::filesystem::path shaderDirectory);
        ~ObjRenderer();

        ObjRenderer(const ObjRenderer&) = delete;
        ObjRenderer& operator=(const ObjRenderer&) = delete;

        void drawFrame(RenderSettings& settings);
        void waitIdle() const;

    private:
        static constexpr std::uint32_t maxFramesInFlight = 2;

        struct SwapchainSupport {
            VkSurfaceCapabilitiesKHR capabilities{};
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };

        struct FrameUniforms {
            glm::mat4 model{1.0f};
            glm::mat4 view{1.0f};
            glm::mat4 projection{1.0f};
            glm::vec4 cameraPosition{0.0f};
            glm::vec4 lightPosition{0.0f};
            glm::vec4 lightColor{1.0f};
            glm::vec4 materialColor{1.0f};
            glm::vec4 materialParams{0.12f, 0.55f, 48.0f, 0.0f};
        };

        void createSwapchainResources();
        void cleanupSwapchainResources();
        void recreateSwapchain();

        void createSwapchain();
        void createImageViews();
        void createDescriptorSetLayout();
        void createGraphicsPipeline();
        void createDepthResources();
        void createCommandBuffers();
        void createSyncObjects();
        void createMeshBuffers();
        void createUniformBuffers();
        void createDescriptorPool();
        void createDescriptorSets();
        void initImGui();
        void shutdownImGui();

        void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);
        void recordScenePass(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);
        void updateUniformBuffer(std::uint32_t frameIndex, const RenderSettings& settings);
        void drawSettingsUi(RenderSettings& settings);

        [[nodiscard]] SwapchainSupport querySwapchainSupport() const;
        [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
        [[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
        [[nodiscard]] VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
        [[nodiscard]] VkFormat findDepthFormat() const;

        platform::Window& window_;
        VulkanContext& context_;
        VulkanResourceManager resources_;
        ShaderLibrary shaders_;
        PipelineFactory pipelineFactory_;
        ImGuiLayer imgui_;
        FrameGraph frameGraph_;
        const assets::Mesh& mesh_;

        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkFormat swapchainImageFormat_ = VK_FORMAT_UNDEFINED;
        VkExtent2D swapchainExtent_{};
        std::uint32_t minImageCount_ = 2;
        std::vector<VkImage> swapchainImages_;
        std::vector<VkImageView> swapchainImageViews_;

        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        GraphicsPipeline graphicsPipeline_;

        VulkanImage depthImage_;
        VulkanBuffer vertexBuffer_;
        VulkanBuffer indexBuffer_;
        std::array<VulkanBuffer, maxFramesInFlight> uniformBuffers_{};

        VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, maxFramesInFlight> descriptorSets_{};

        std::array<VkCommandBuffer, maxFramesInFlight> commandBuffers_{};
        std::array<VkSemaphore, maxFramesInFlight> imageAvailableSemaphores_{};
        std::array<VkSemaphore, maxFramesInFlight> renderFinishedSemaphores_{};
        std::array<VkFence, maxFramesInFlight> inFlightFences_{};
        std::uint32_t currentFrame_ = 0;

        glm::vec3 meshCenter_{0.0f};
        float meshScale_ = 1.0f;
    };

} // namespace lumin::render

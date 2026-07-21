#include "lumin/render/ImGuiLayer.hpp"

#include "lumin/platform/Window.hpp"

#include <iterator>
#include <stdexcept>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

namespace lumin::render {
    namespace {

        void checkVk(VkResult result) {
            if (result != VK_SUCCESS) {
                throw std::runtime_error("Dear ImGui Vulkan backend reported a Vulkan error.");
            }
        }

    } // namespace

    ImGuiLayer::~ImGuiLayer() {
        shutdown();
    }

    void ImGuiLayer::initialize(platform::Window& window, const ImGuiLayerConfig& config) {
        shutdown();

        device_ = config.device;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        if (config.enableKeyboard) {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
        if (config.enableGamepad) {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        }
        // 当前 vcpkg 默认 imgui port 未启用 docking 分支；字段先保留在配置层，方便后续切 feature。
        (void)config.enableDocking;
        io.FontGlobalScale = config.globalScale;
        ImGui::StyleColorsDark();

        VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, config.samplerDescriptorCount},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, config.sampledImageDescriptorCount},
        };

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = config.maxDescriptorSets;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;

        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create ImGui descriptor pool.");
        }

        ImGui_ImplGlfw_InitForVulkan(window.nativeHandle(), true);

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &config.colorFormat;
        renderingInfo.depthAttachmentFormat = config.depthFormat;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = config.apiVersion;
        initInfo.Instance = config.instance;
        initInfo.PhysicalDevice = config.physicalDevice;
        initInfo.Device = config.device;
        initInfo.QueueFamily = config.queueFamily;
        initInfo.Queue = config.queue;
        initInfo.DescriptorPool = descriptorPool_;
        initInfo.MinImageCount = config.minImageCount;
        initInfo.ImageCount = config.imageCount;
        initInfo.PipelineInfoMain.MSAASamples = config.samples;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
        initInfo.UseDynamicRendering = true;
        initInfo.CheckVkResultFn = checkVk;

        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("Failed to initialize Dear ImGui Vulkan backend.");
        }

        initialized_ = true;
    }

    void ImGuiLayer::shutdown() {
        if (!initialized_) {
            return;
        }

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        initialized_ = false;

        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
        }
        device_ = VK_NULL_HANDLE;
    }

    void ImGuiLayer::newFrame() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::render(VkCommandBuffer commandBuffer) {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    bool ImGuiLayer::initialized() const noexcept {
        return initialized_;
    }

} // namespace lumin::render

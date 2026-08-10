#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>

#include "render/DeferredRenderPipeline.hpp"
#include "render/FrameGraph.hpp"
#include "render/ImGuiManager.hpp"
#include "render/ModelRenderer.hpp"
#include "render/PipelineManager.hpp"
#include "render/RenderSettings.hpp"
#include "render/TextureManager.hpp"
#include "render/atmosphere/AtmosphereLutGpu.hpp"
#include "render/atmosphere/AtmosphereLutScheduler.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include "render/world/RenderWorld.hpp"

namespace lumin::platform {
    class Window;
}

namespace lumin::scene {
    class Camera;
    class Level;
} // namespace lumin::scene

namespace lumin::render {

    class VulkanContext;

    /**
     * @brief 协调渲染世界快照、延迟渲染 Feature、交换链提交和跨帧状态。
     *
     * 该类型不直接拥有 barrier；所有资源状态转换由 `FrameGraph` 声明。跨帧状态只在
     * `VulkanContext::submitFrameCommands()` 成功返回后统一提交；present 失败不得回滚 GPU 历史。
     */
    class LevelRenderer {
    public:
        /** 创建渲染器并从 `level` 提取首份不可变渲染世界快照。 */
        LevelRenderer(platform::Window& window, VulkanContext& context, const scene::Level& level,
                      std::filesystem::path shaderDirectory,
                      std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination = {});
        ~LevelRenderer();

        LevelRenderer(const LevelRenderer&) = delete;
        LevelRenderer& operator=(const LevelRenderer&) = delete;

        /// 开始 UI 帧；通常由编辑器在构建控件前调用。
        void beginUiFrame(ImGuiContent* content = nullptr);
        /// 放弃尚未录制的 UI 帧。
        void cancelUiFrame() noexcept;
        /// 提取场景变化、构建 Feature 管线并提交一帧。
        void drawFrame(scene::Camera& camera, RenderSettings& settings, ImGuiContent* content = nullptr);
        /// 等待当前设备队列空闲。
        void waitIdle() const;

        [[nodiscard]] std::uint32_t modelCount() const noexcept;
        [[nodiscard]] std::uint32_t mdiDrawCount() const noexcept;
        [[nodiscard]] gi::BackendInfo globalIlluminationBackendInfo() const noexcept;
        [[nodiscard]] ImGuiCaptureState imguiCaptureState() const noexcept;
        /** 请求按 Viewport 内容区物理像素重建渲染资源。连续 resize 会等待尺寸稳定后应用。 */
        void requestViewportExtent(std::uint32_t width, std::uint32_t height) noexcept;
        [[nodiscard]] ImGuiViewportImage viewportImage() const noexcept;

    private:
        struct FeatureConfigurationState {
            GlobalIlluminationMode globalIlluminationMode = GlobalIlluminationMode::RayTracing;
            bool directLightingEnabled = true;
            bool shadowsEnabled = true;
            float shadowSplitLambda = 0.68f;
            float shadowMaxDistance = 200.0f;
            bool ssaoEnabled = true;
            bool sharcEnabled = true;
            bool nrdEnabled = true;
            bool temporalAaEnabled = true;
            bool atmosphereEnabled = true;
            bool aerialPerspectiveEnabled = true;

            friend bool operator==(const FeatureConfigurationState&, const FeatureConfigurationState&) = default;
        };

        struct RecordedFrameState {
            glm::mat4 viewProjection{1.0f};
            glm::mat4 view{1.0f};
            glm::mat4 projection{1.0f};
            glm::vec2 jitter{0.0f};
            FeatureConfigurationState featureConfiguration;
            bool usedHybridGlobalIllumination = false;
        };

        struct HybridGiState;

        void createRenderResources();
        void createRenderFeaturePipeline(DeferredRenderPath requestedPath = DeferredRenderPath::Hybrid);
        void synchronizeRenderConfiguration(const RenderSettings& settings);
        void createModelRenderer();
        void createDirectLightingBindingLayout();
        void createDirectLightingBindingSets();
        void createAtmosphereConsumerBindings();
        void createHybridGiResources();
        void ensureHybridGiCapacity();
        void destroyDirectLightingBindings() noexcept;
        void destroyHybridGiResources() noexcept;
        void destroyRenderResources() noexcept;
        void refreshSwapchainResources();
        void applyPendingViewportExtent();
        void createViewportOutput();
        void commitSubmittedRuntimeFrame(const core::RenderFrameIdentity& identity);
        void discardPendingRuntimeFrame() noexcept;
        [[nodiscard]] RecordedFrameState recordCommandList(nvrhi::ICommandList& commandList,
                                                           const core::RenderFrameIdentity& identity,
                                                           const scene::Camera& camera, const RenderSettings& settings,
                                                           world::SceneChangeMask sceneChanges,
                                                           const core::FrameChangeSet& changes);
        void addShadowFeaturePasses(core::RenderFeatureFrameContext& context);
        void addGBufferFeaturePasses(core::RenderFeatureFrameContext& context);
        void addHybridSurfaceFeaturePasses(core::RenderFeatureFrameContext& context);
        void addAtmosphereLutFeaturePasses(core::RenderFeatureFrameContext& context);
        void addGlobalIlluminationFeaturePasses(core::RenderFeatureFrameContext& context);
        void addGiDenoiserFeaturePasses(core::RenderFeatureFrameContext& context);
        void addSkyCompositeFeaturePasses(core::RenderFeatureFrameContext& context);
        void addDirectLightingFeaturePasses(core::RenderFeatureFrameContext& context);
        void addTemporalAaFeaturePasses(core::RenderFeatureFrameContext& context);
        void addToneMappingFeaturePasses(core::RenderFeatureFrameContext& context);
        void addUiPresentFeaturePasses(core::RenderFeatureFrameContext& context);
        [[nodiscard]] static FeatureConfigurationState featureConfiguration(const RenderSettings& settings) noexcept;
        [[nodiscard]] bool shouldUseHybridGi(const RenderSettings& settings,
                                             const world::RenderWorldSnapshot& renderWorld) const noexcept;
        void recordShadowPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                              std::uint32_t frameIndex, std::uint32_t cascadeIndex,
                              const glm::mat4& lightViewProjection);
        void recordGBufferPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                               std::uint32_t frameIndex, const glm::mat4& viewProjection,
                               const glm::mat4& previousViewProjection);
        void recordFullscreenPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                  const nvrhi::GraphicsPipelineHandle& pipeline, std::uint32_t frameIndex,
                                  const nvrhi::BindingSetHandle& additionalBindingSet = {});
        void recordDirectLightingPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                      std::uint32_t frameIndex);
        void recordHistoryCopy(nvrhi::ICommandList& commandList, std::uint32_t frameIndex);

        platform::Window& window_;
        VulkanContext& context_;
        const scene::Level& level_;
        std::filesystem::path shaderDirectory_;
        TextureManager textures_;
        PipelineManager pipelines_;
        std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination_;
        ImGuiManager imgui_;
        GpuTexture viewportOutput_;
        core::RenderExtent renderExtent_{1, 1};
        core::RenderExtent requestedRenderExtent_{1, 1};
        std::uint32_t requestedExtentStableFrames_ = 0;
        bool viewportOutputInitialized_ = false;
        FrameGraph frameGraph_;
        world::RenderWorldCache renderWorld_;
        atmosphere::AtmosphereLutScheduler atmosphereLutScheduler_;
        std::unique_ptr<atmosphere::AtmosphereLutGpu> atmosphereLutGpu_;
        std::optional<core::RenderSequence> pendingAtmosphereSequence_;
        nvrhi::BindingLayoutHandle atmosphereConsumerBindingLayout_;
        std::array<nvrhi::BindingSetHandle, TextureManager::maxFramesInFlight> atmosphereConsumerBindingSets_{};
        /// RT、SHARC、NRD 与 GPU Scene 的实现细节隔离在 cpp，避免公共头传播 vendor 依赖。
        std::unique_ptr<HybridGiState> hybridGi_;
        std::unique_ptr<ModelRenderer> modelRenderer_;
        nvrhi::BindingLayoutHandle directLightingBindingLayout_;
        std::array<nvrhi::BindingSetHandle, TextureManager::maxFramesInFlight> directLightingBindingSets_{};
        std::uint64_t swapchainGeneration_ = 0;
        glm::mat4 previousViewProjection_{1.0f};
        glm::mat4 previousView_{1.0f};
        glm::mat4 previousProjection_{1.0f};
        glm::vec2 previousJitter_{0.0f};
        FeatureConfigurationState committedFeatureConfiguration_{};
        bool hasSubmittedFrame_ = false;
        bool lastSubmittedFrameUsedHybridGi_ = false;
        std::uint64_t nextRenderSequence_ = 0;
        core::FrameChangeSet pendingFrameChanges_;
        std::array<bool, TextureManager::maxFramesInFlight> frameResourcesInitialized_{};
        bool atmosphereForceRebuild_ = true;
        bool requestedSharcEnabled_ = true;
        std::unique_ptr<DeferredRenderPipeline> renderPipeline_;
    };

} // namespace lumin::render

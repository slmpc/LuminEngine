#pragma once

#include "render/DeferredRenderPipeline.hpp"
#include "render/resources/FrameGraph.hpp"
#include "render/editor/ImGuiManager.hpp"
#include "render/LevelRenderer.hpp"
#include "render/level/LevelRenderFrameData.hpp"
#include "render/ModelRenderer.hpp"
#include "render/resources/PipelineManager.hpp"
#include "render/resources/TextureManager.hpp"
#include "render/atmosphere/AtmosphereLutGpu.hpp"
#include "render/atmosphere/AtmosphereLutScheduler.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include "render/world/RenderWorld.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <vulkan/vulkan.h>

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
#include "render/gi/HybridLightingComposite.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#endif

namespace lumin::render {

    struct LevelRenderer::Impl final : LevelRenderFeatureHost {
    public:
        Impl(platform::Window& window, VulkanContext& context, const scene::Level& level,
             std::filesystem::path shaderDirectory,
             std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination);
        ~Impl();

        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;

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

        struct HybridGiState {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
            bool sharcEnabled = true;
            std::unique_ptr<gpu::NvrhiGpuSceneBackend> sceneBackend;
            std::unique_ptr<gpu::GpuSceneResources> sceneResources;
            std::unique_ptr<gpu::GpuSceneUpdatePlanner> scenePlanner;
            std::unique_ptr<gi::RayTracedDirectLightingPass> directLighting;
            std::unique_ptr<gi::SharcRadianceCache> sharc;
            std::unique_ptr<gi::RayTracedGiPass> rayTracedGi;
            std::unique_ptr<gi::NrdDenoiser> nrd;
            std::unique_ptr<gi::GiCompositePass> composite;
            std::unique_ptr<gi::HybridLightingCompositePass> lightingComposite;
            std::array<gi::RayTracedDiFrameResources, TextureManager::maxFramesInFlight> directLightingFrames{};
            std::optional<gpu::GpuSceneUpdatePlan> pendingScenePlan;
            std::optional<gpu::GpuScenePreparedUpdate> pendingSceneUpdate;
            std::optional<gi::NrdPreparedFrame> pendingNrdFrame;
            std::optional<core::RenderSequence> pendingSequence;
            std::uint32_t geometryDescriptorCapacity = 0;
#endif
        };

        void beginUiFrame(ImGuiContent* content = nullptr);
        void cancelUiFrame() noexcept;
        void drawFrame(scene::Camera& camera, RenderSettings& settings, ImGuiContent* content = nullptr);
        void waitIdle() const;
        [[nodiscard]] std::uint32_t modelCount() const noexcept;
        [[nodiscard]] std::uint32_t mdiDrawCount() const noexcept;
        [[nodiscard]] gi::BackendInfo globalIlluminationBackendInfo() const noexcept;
        [[nodiscard]] ImGuiCaptureState imguiCaptureState() const noexcept;
        void requestViewportExtent(std::uint32_t width, std::uint32_t height) noexcept;
        [[nodiscard]] ImGuiViewportImage viewportImage() const noexcept;

        void addFeaturePasses(LevelRenderFeatureKind kind, core::RenderFeatureFrameContext& context) override;
        void submitFeature(LevelRenderFeatureKind kind,
                           const core::RenderFrameIdentity& identity) noexcept override;
        void discardFeature(LevelRenderFeatureKind kind,
                            const core::RenderFrameIdentity& identity) noexcept override;

    private:
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
        void commitAtmosphereFeature(const core::RenderFrameIdentity& identity) noexcept;
        void commitHybridSurfaceFeature(const core::RenderFrameIdentity& identity) noexcept;
        void commitGlobalIlluminationFeature(const core::RenderFrameIdentity& identity) noexcept;
        void commitGiDenoiserFeature(const core::RenderFrameIdentity& identity) noexcept;
        void discardAtmosphereFeature() noexcept;
        void discardHybridSurfaceFeature() noexcept;
        void discardGlobalIlluminationFeature() noexcept;
        void discardGiDenoiserFeature() noexcept;
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

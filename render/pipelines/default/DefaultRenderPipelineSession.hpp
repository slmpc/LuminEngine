#pragma once

#include "render/ModelRenderer.hpp"
#include "render/atmosphere/AtmosphereLutGpu.hpp"
#include "render/atmosphere/AtmosphereLutScheduler.hpp"
#include "render/core/RenderPipelineInstance.hpp"
#include "render/features/postfx/PostFxResources.hpp"
#include "render/features/raster/RasterFeatureResources.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include "render/level/FeatureFrameData.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"
#include "render/presentation/PresentationRenderer.hpp"
#include "render/resources/FrameGraph.hpp"
#include "render/resources/FullscreenPipelineFactory.hpp"
#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"
#include "render/resources/VulkanResources.hpp"
#include "render/runtime/RenderPipelineSession.hpp"
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
#include "render/gi/raytracing/HybridLightingComposite.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#endif

namespace lumin::render::pipelines {

    /** 内置 Raster/Hybrid recipe 的具体 Pipeline session；只由默认 session factory 创建。 */
    class DefaultRenderPipelineSession final : public runtime::IRenderPipelineSession {
    public:
        /// 迁移期 Runtime 与 VulkanContext 共用的并行帧槽数量。
        static constexpr std::uint32_t frameSlotCount = 2;

        DefaultRenderPipelineSession(VulkanContext& context, world::RenderWorldSnapshotPtr initialWorld,
                                     std::filesystem::path shaderDirectory, ImFontAtlas& uiFontAtlas,
                                     std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination = {});
        ~DefaultRenderPipelineSession() override;

        DefaultRenderPipelineSession(const DefaultRenderPipelineSession&) = delete;
        DefaultRenderPipelineSession& operator=(const DefaultRenderPipelineSession&) = delete;

        struct FeatureConfigurationState {
            GlobalIlluminationMode globalIlluminationMode = GlobalIlluminationMode::RayTracing;
            bool directLightingEnabled = true;
            bool shadowsEnabled = true;
            float shadowSplitLambda = 0.68f;
            float shadowMaxDistance = 200.0f;
            bool ssaoEnabled = true;
            AmbientOcclusionMode ambientOcclusionMode = AmbientOcclusionMode::Ssao;
            float ambientOcclusionRadius = 1.0f;
            float ambientOcclusionStrength = 1.0f;
            float ambientOcclusionBias = 0.08f;
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
            bool usedHybridPath = false;
            bool usedDirectNrd = false;
            bool usedIndirectLighting = false;
        };

        struct HybridGiState {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
            std::unique_ptr<gpu::NvrhiGpuSceneBackend> sceneBackend;
            std::unique_ptr<gpu::GpuSceneResources> sceneResources;
            std::unique_ptr<gpu::GpuSceneUpdatePlanner> scenePlanner;
            std::unique_ptr<gi::RayTracedDirectLightingPass> directLighting;
            std::unique_ptr<gi::HybridLightingCompositePass> lightingComposite;
            std::array<gi::RayTracedDiFrameResources, frameSlotCount> directLightingFrames{};
            std::optional<gpu::GpuSceneUpdatePlan> pendingScenePlan;
            std::optional<gpu::GpuScenePreparedUpdate> pendingSceneUpdate;
            std::optional<core::RenderSequence> pendingSequence;
            std::uint32_t geometryDescriptorCapacity = 0;
#if LUMIN_LEVEL_RENDERER_HAS_NRD
            std::unique_ptr<gi::RtDiNrdInputsPass> directNrdInputs;
            std::unique_ptr<gi::NrdDenoiser> directNrd;
            std::unique_ptr<gi::GiCompositePass> directComposite;
            std::optional<gi::NrdPreparedFrame> pendingDirectNrdFrame;
#endif
#if LUMIN_LEVEL_RENDERER_HAS_SHARC_INDIRECT
            bool sharcEnabled = false;
            std::unique_ptr<gi::SharcRadianceCache> sharc;
            std::unique_ptr<gi::SharcIndirectLightingPass> indirectLighting;
            std::unique_ptr<gi::NrdDenoiser> indirectNrd;
            std::unique_ptr<gi::GiCompositePass> indirectComposite;
            std::optional<gi::NrdPreparedFrame> pendingIndirectNrdFrame;
#endif
#endif
        };

        [[nodiscard]] bool drawFrame(core::RenderFramePacket packet, const ImDrawData& ui) override;
        void waitIdle() const override;
        [[nodiscard]] runtime::RenderPipelineSessionStatus status() const override;
        [[nodiscard]] std::uint32_t modelCount() const noexcept;
        [[nodiscard]] std::uint32_t mdiDrawCount() const noexcept;
        [[nodiscard]] gi::BackendInfo globalIlluminationBackendInfo() const noexcept;
        [[nodiscard]] const std::string& diagnostic() const noexcept;

    private:
        class FeatureModuleBase;
        class AtmosphereFeatureModule;
        class ShadowFeatureModule;
        class RasterSurfaceFeatureModule;
        class HybridSurfaceFeatureModule;
        class GlobalIlluminationFeatureModule;
        class DenoisingFeatureModule;
        class LightingCompositeFeatureModule;
        class TemporalAaFeatureModule;
        class ToneMappingFeatureModule;
        class PresentationFeatureModule;

        void createRenderResources();
        void createRenderFeaturePipeline(DefaultRenderPipelineKind requestedPath = DefaultRenderPipelineKind::Hybrid);
        [[nodiscard]] core::RenderFeatureRegistry
        createFeatureRegistry(const DefaultRenderPipelineDefinition& definition, DefaultRenderPipelineKind path);
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
        void requestViewportExtent(std::uint32_t width, std::uint32_t height) noexcept;
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
        [[nodiscard]] RecordedFrameState
        recordCommandList(nvrhi::ICommandList& commandList, const core::RenderFrameIdentity& identity,
                          const core::RenderFramePacket& packet, const RenderSettings& settings,
                          world::SceneChangeMask sceneChanges, const core::FrameChangeSet& changes);
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
        [[nodiscard]] bool shouldUseHybridGi(const GlobalIlluminationSettings& settings,
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

        VulkanContext& context_;
        std::filesystem::path shaderDirectory_;
        ImFontAtlas* uiFontAtlas_ = nullptr;
        /** 仅在同步 `drawFrame()` 调用期间有效。 */
        const ImDrawData* currentUiDrawData_ = nullptr;
        RasterFeatureResources rasterResources_;
        PostFxResources postFxResources_;
        GpuResourceManager resourceFactory_;
        ShaderLibrary shaderLibrary_;
        PipelineFactory pipelineFactory_;
        FullscreenPipelineFactory fullscreenPipelineFactory_;
        nvrhi::GraphicsPipelineHandle skyPipeline_;
        nvrhi::GraphicsPipelineHandle directLightingPipeline_;
        nvrhi::GraphicsPipelineHandle temporalAaPipeline_;
        nvrhi::GraphicsPipelineHandle toneMappingPipeline_;
        std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination_;
        PresentationRenderer presentation_;
        GpuTexture viewportOutput_;
        core::RenderExtent renderExtent_{1, 1};
        core::RenderExtent requestedRenderExtent_{1, 1};
        std::uint32_t requestedExtentStableFrames_ = 0;
        bool viewportOutputInitialized_ = false;
        FrameGraph frameGraph_;
        // currentWorld_ 可随录制尝试改变；committedWorld_ 只在 GPU submit 成功后推进。
        world::RenderWorldSnapshotPtr currentWorld_;
        world::RenderWorldSnapshotPtr committedWorld_;
        atmosphere::AtmosphereLutScheduler atmosphereLutScheduler_;
        std::unique_ptr<atmosphere::AtmosphereLutGpu> atmosphereLutGpu_;
        std::optional<core::RenderSequence> pendingAtmosphereSequence_;
        nvrhi::BindingLayoutHandle atmosphereConsumerBindingLayout_;
        std::array<nvrhi::BindingSetHandle, frameSlotCount> atmosphereConsumerBindingSets_{};
        std::unique_ptr<HybridGiState> hybridGi_;
        std::unique_ptr<ModelRenderer> modelRenderer_;
        nvrhi::BindingLayoutHandle directLightingBindingLayout_;
        std::array<nvrhi::BindingSetHandle, frameSlotCount> directLightingBindingSets_{};
        std::uint64_t swapchainGeneration_ = 0;
        glm::mat4 previousViewProjection_{1.0f};
        glm::mat4 previousView_{1.0f};
        glm::mat4 previousProjection_{1.0f};
        glm::vec2 previousJitter_{0.0f};
        FeatureConfigurationState committedFeatureConfiguration_{};
        core::RenderSettingsSchemaRegistry settingsSchemas_;
        std::optional<core::RenderSettingsSnapshot> committedSettings_;
        bool hasSubmittedFrame_ = false;
        bool lastSubmittedFrameUsedHybridPath_ = false;
        std::uint64_t nextRenderSequence_ = 0;
        core::FrameChangeSet pendingFrameChanges_;
        std::array<bool, frameSlotCount> frameResourcesInitialized_{};
        /// RTDI 合并直接光由成功 Hybrid 提交单独发布，不能沿用 Raster/PostFX 的粗粒度状态。
        std::array<bool, frameSlotCount> directRadianceInitialized_{};
        /// Direct NRD composite 只在 NRD 实际启用并成功提交后进入 ShaderResource 状态。
        std::array<bool, frameSlotCount> directNrdOutputInitialized_{};
        /// Raster GI 或 SHARC composite 成功写入后，间接光纹理才可声明为 ShaderResource。
        std::array<bool, frameSlotCount> globalIlluminationInitialized_{};
        bool atmosphereForceRebuild_ = true;
        bool requestedSharcEnabled_ = true;
        DefaultRenderPipelineKind activePipelineKind_ = DefaultRenderPipelineKind::Raster;
        std::unique_ptr<core::RenderPipelineInstance> renderPipeline_;
        std::string diagnostic_;
    };

} // namespace lumin::render::pipelines

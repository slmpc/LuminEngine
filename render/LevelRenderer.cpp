#include "render/level/LevelRenderFrameData.hpp"
#include "render/level/LevelRendererImpl.hpp"

#include "render/gi/legacy/LegacyBackend.hpp"
#include "render/gpu/GpuScene.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/Camera.hpp"
#include "scene/Level.hpp"

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
#include "render/gi/raytracing/HybridLightingComposite.hpp"
#include "render/gi/raytracing/RayTracedDirectLighting.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/trigonometric.hpp>

namespace lumin::render {

    LevelRenderer::LevelRenderer(VulkanContext& context, const scene::Level& level,
                                 std::filesystem::path shaderDirectory, core::UiFontAtlas uiFontAtlas,
                                 std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination)
        : impl_(std::make_unique<Impl>(context, level, std::move(shaderDirectory), std::move(uiFontAtlas),
                                       std::move(globalIllumination))) {
    }

    LevelRenderer::~LevelRenderer() = default;

    void LevelRenderer::drawFrame(scene::Camera& camera, RenderSettings& settings, core::UiDrawPacket uiDrawPacket) {
        impl_->drawFrame(camera, settings, std::move(uiDrawPacket));
    }

    void LevelRenderer::waitIdle() const {
        impl_->waitIdle();
    }

    std::uint32_t LevelRenderer::modelCount() const noexcept {
        return impl_->modelCount();
    }

    std::uint32_t LevelRenderer::mdiDrawCount() const noexcept {
        return impl_->mdiDrawCount();
    }

    gi::BackendInfo LevelRenderer::globalIlluminationBackendInfo() const noexcept {
        return impl_->globalIlluminationBackendInfo();
    }

    void LevelRenderer::requestViewportExtent(std::uint32_t width, std::uint32_t height) noexcept {
        impl_->requestViewportExtent(width, height);
    }

    ImGuiViewportImage LevelRenderer::viewportImage() const noexcept {
        return impl_->viewportImage();
    }

    LevelRenderer::Impl::Impl(VulkanContext& context, const scene::Level& level, std::filesystem::path shaderDirectory,
                              core::UiFontAtlas uiFontAtlas,
                              std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination)
        : context_(context), level_(level), shaderDirectory_(std::move(shaderDirectory)),
          uiFontAtlas_(std::move(uiFontAtlas)), textures_(context), pipelines_(context, shaderDirectory_),
          globalIllumination_(std::move(globalIllumination)) {
        if (globalIllumination_ == nullptr) {
            globalIllumination_ = gi::makeLegacyBackend(shaderDirectory_);
        }
        renderExtent_ = core::RenderExtent{context_.swapchainWidth(), context_.swapchainHeight()};
        requestedRenderExtent_ = renderExtent_;
        static_cast<void>(renderWorld_.sync(level_));
        createRenderResources();
        // 先创建 RT 资源，再根据真实的 device/scene capability 选择固定的帧图拓扑。
        createRenderFeaturePipeline();
        presentation_.initialize(context_, uiFontAtlas_, shaderDirectory_);
        presentation_.setViewportTexture(viewportOutput_.texture);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    LevelRenderer::Impl::~Impl() {
        waitIdle();
        presentation_.shutdown();
        destroyRenderResources();
    }

    void LevelRenderer::Impl::drawFrame(scene::Camera& camera, RenderSettings& settings,
                                        core::UiDrawPacket uiDrawPacket) {
        if (swapchainGeneration_ != context_.swapchainGeneration()) {
            refreshSwapchainResources();
        }
        applyPendingViewportExtent();
        requestedSharcEnabled_ = settings.globalIllumination.sharcEnabled;
        const world::SceneDelta sceneDelta = renderWorld_.sync(level_);
        pendingFrameChanges_.merge(core::frameChangesFromScene(sceneDelta.changes));
        const world::SceneChangeMask rebuildChanges = world::SceneChangeMask::Geometry |
                                                      world::SceneChangeMask::InstanceTopology |
                                                      world::SceneChangeMask::MaterialBinding;
        if (sceneDelta.has(rebuildChanges)) {
            context_.waitIdle();
            frameGraph_.reset();
            directLightingBindingSets_.fill(nullptr);
            modelRenderer_.reset();
            createModelRenderer();
            ensureHybridGiCapacity();
        }
        synchronizeRenderConfiguration(settings);

        const FeatureConfigurationState currentConfiguration = featureConfiguration(settings);
        if (hasSubmittedFrame_ && currentConfiguration != committedFeatureConfiguration_) {
            pendingFrameChanges_.add(core::HistoryReason::FeatureConfigurationChanged);
        }

        if (nextRenderSequence_ == core::RenderSequence::invalidValue) {
            throw std::overflow_error("LevelRenderer exhausted the logical render sequence range.");
        }

        std::optional<VulkanFrame> frame;
        try {
            frame = context_.beginFrame();
        } catch (...) {
            throw;
        }
        if (!frame.has_value()) {
            refreshSwapchainResources();
            return;
        }

        const core::RenderFrameIdentity identity{
            core::FrameSlotIndex{frame->frameIndex},
            core::SwapImageIndex{frame->imageIndex},
            core::RenderSequence{nextRenderSequence_},
            renderExtent_,
        };
        RecordedFrameState recorded;
        try {
            recorded = recordCommandList(*frame->commandList, identity, camera, settings, uiDrawPacket,
                                         sceneDelta.changes, pendingFrameChanges_);
        } catch (...) {
            const std::exception_ptr recordingFailure = std::current_exception();
            renderPipeline_->discardFrame();
            try {
                // cancelFrame 已消费 acquire semaphore 并更新 swapchain generation；下一帧入口负责统一重建。
                context_.cancelFrame(*frame);
            } catch (...) {
                // 清理失败不得覆盖最初的录制异常；当前帧之后由上层决定退出或恢复设备。
            }
            std::rethrow_exception(recordingFailure);
        }

        try {
            context_.submitFrameCommands(*frame);
        } catch (...) {
            renderPipeline_->discardFrame();
            throw;
        }

        renderPipeline_->commitFrame(identity);
        frameResourcesInitialized_[frame->frameIndex] = true;
        previousViewProjection_ = recorded.viewProjection;
        previousView_ = recorded.view;
        previousProjection_ = recorded.projection;
        previousJitter_ = recorded.jitter;
        committedFeatureConfiguration_ = recorded.featureConfiguration;
        hasSubmittedFrame_ = true;
        lastSubmittedFrameUsedHybridGi_ = recorded.usedHybridGlobalIllumination;
        pendingFrameChanges_.clear();
        ++nextRenderSequence_;

        const bool recreate = context_.presentFrame(*frame);
        if (recreate) {
            refreshSwapchainResources();
        }
    }

    void LevelRenderer::Impl::waitIdle() const {
        context_.waitIdle();
    }

    void LevelRenderer::Impl::requestViewportExtent(std::uint32_t width, std::uint32_t height) noexcept {
        if (width == 0 || height == 0) {
            return;
        }
        constexpr std::uint32_t maximumViewportDimension = 16'384;
        const core::RenderExtent requested{std::clamp(width, 1U, maximumViewportDimension),
                                           std::clamp(height, 1U, maximumViewportDimension)};
        if (requested != requestedRenderExtent_) {
            requestedRenderExtent_ = requested;
            requestedExtentStableFrames_ = 1;
            return;
        }
        if (requestedExtentStableFrames_ < std::numeric_limits<std::uint32_t>::max()) {
            ++requestedExtentStableFrames_;
        }
    }

    ImGuiViewportImage LevelRenderer::Impl::viewportImage() const noexcept {
        return {PresentationRenderer::viewportTextureId(), viewportOutput_.width, viewportOutput_.height};
    }

    std::uint32_t LevelRenderer::Impl::modelCount() const noexcept {
        return modelRenderer_ == nullptr ? 0 : modelRenderer_->drawCount();
    }

    std::uint32_t LevelRenderer::Impl::mdiDrawCount() const noexcept {
        return modelCount();
    }

    gi::BackendInfo LevelRenderer::Impl::globalIlluminationBackendInfo() const noexcept {
        if (lastSubmittedFrameUsedHybridGi_) {
            if (committedFeatureConfiguration_.sharcEnabled && committedFeatureConfiguration_.nrdEnabled) {
                return gi::BackendInfo{"Ray Tracing + SHARC + NRD", true, true};
            }
            if (committedFeatureConfiguration_.sharcEnabled) {
                return gi::BackendInfo{"Ray Tracing + SHARC", true, true};
            }
            if (committedFeatureConfiguration_.nrdEnabled) {
                return gi::BackendInfo{"Ray Tracing + NRD", true, true};
            }
            return gi::BackendInfo{"Ray Tracing", false, true};
        }
        return globalIllumination_->info();
    }

    void LevelRenderer::Impl::createRenderFeaturePipeline(DeferredRenderPath requestedPath) {
        core::RenderDeviceCapabilities capabilities;
        capabilities.supported = {core::RenderCapability::Graphics, core::RenderCapability::Compute,
                                  core::RenderCapability::DynamicRendering};
        capabilities.maxFramesInFlight = TextureManager::maxFramesInFlight;
        DeferredRenderPath path = DeferredRenderPath::Raster;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        const world::RenderWorldSnapshotPtr snapshot = renderWorld_.snapshot();
        const bool hybridPath = requestedPath == DeferredRenderPath::Hybrid && hybridGi_ != nullptr &&
                                context_.rayTracingDecision().enabled() &&
                                context_.rayTracingSupport().supportsSharcShaderStorage() && snapshot != nullptr &&
                                !snapshot->instances().empty() && !snapshot->meshes().empty();
        if (hybridPath) {
            path = DeferredRenderPath::Hybrid;
            capabilities.supported.add(core::RenderCapability::DescriptorIndexing)
                .add(core::RenderCapability::BufferDeviceAddress)
                .add(core::RenderCapability::AccelerationStructure)
                .add(core::RenderCapability::RayTracingPipeline)
                .add(core::RenderCapability::Nrd)
                .add(core::RenderCapability::Sharc);
        }
#else
        static_cast<void>(requestedPath);
#endif
        DeferredRenderFeatureSet features;
        const auto makeFeature = [this, path](LevelRenderFeatureKind kind) {
            return makeLevelRenderFeature(kind, deferredFeatureDescriptor(kind, path), *this);
        };
        if (path == DeferredRenderPath::Raster) {
            features.shadow = makeFeature(LevelRenderFeatureKind::Shadow);
            features.gbuffer = makeFeature(LevelRenderFeatureKind::GBuffer);
        } else {
            features.hybridSurface = makeFeature(LevelRenderFeatureKind::HybridSurface);
        }
        features.atmosphereLuts = makeFeature(LevelRenderFeatureKind::AtmosphereLuts);
        features.globalIllumination = makeFeature(LevelRenderFeatureKind::GlobalIllumination);
        features.giDenoiser = makeFeature(LevelRenderFeatureKind::GiDenoiser);
        features.skyComposite = makeFeature(LevelRenderFeatureKind::SkyComposite);
        features.directLighting = makeFeature(LevelRenderFeatureKind::DirectLighting);
        features.temporalAa = makeFeature(LevelRenderFeatureKind::TemporalAa);
        features.toneMapping = makeFeature(LevelRenderFeatureKind::ToneMapping);
        features.uiPresent = makeFeature(LevelRenderFeatureKind::UiPresent);
        renderPipeline_ = std::make_unique<DeferredRenderPipeline>(std::move(features), capabilities, path);
    }

    void LevelRenderer::Impl::addFeaturePasses(LevelRenderFeatureKind kind, core::RenderFeatureFrameContext& context) {
        switch (kind) {
        case LevelRenderFeatureKind::Shadow:
            addShadowFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::GBuffer:
            addGBufferFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::HybridSurface:
            addHybridSurfaceFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::AtmosphereLuts:
            addAtmosphereLutFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::GlobalIllumination:
            addGlobalIlluminationFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::GiDenoiser:
            addGiDenoiserFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::SkyComposite:
            addSkyCompositeFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::DirectLighting:
            addDirectLightingFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::TemporalAa:
            addTemporalAaFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::ToneMapping:
            addToneMappingFeaturePasses(context);
            return;
        case LevelRenderFeatureKind::UiPresent:
            addUiPresentFeaturePasses(context);
            return;
        }
    }

    void LevelRenderer::Impl::submitFeature(LevelRenderFeatureKind kind,
                                            const core::RenderFrameIdentity& identity) noexcept {
        switch (kind) {
        case LevelRenderFeatureKind::GBuffer:
        case LevelRenderFeatureKind::HybridSurface:
            if (kind == LevelRenderFeatureKind::HybridSurface) {
                commitHybridSurfaceFeature(identity);
            }
            if (modelRenderer_ != nullptr) {
                modelRenderer_->commitSubmittedFrame();
            }
            return;
        case LevelRenderFeatureKind::AtmosphereLuts:
            commitAtmosphereFeature(identity);
            return;
        case LevelRenderFeatureKind::GlobalIllumination:
            commitGlobalIlluminationFeature(identity);
            return;
        case LevelRenderFeatureKind::GiDenoiser:
            commitGiDenoiserFeature(identity);
            return;
        case LevelRenderFeatureKind::TemporalAa:
            textures_.markHistoryValid(identity.frameSlot.value());
            return;
        case LevelRenderFeatureKind::UiPresent:
            viewportOutputInitialized_ = true;
            return;
        default:
            return;
        }
    }

    void LevelRenderer::Impl::discardFeature(LevelRenderFeatureKind kind, const core::RenderFrameIdentity&) noexcept {
        switch (kind) {
        case LevelRenderFeatureKind::GBuffer:
        case LevelRenderFeatureKind::HybridSurface:
            if (kind == LevelRenderFeatureKind::HybridSurface) {
                discardHybridSurfaceFeature();
            }
            if (modelRenderer_ != nullptr) {
                modelRenderer_->discardPendingFrame();
            }
            return;
        case LevelRenderFeatureKind::AtmosphereLuts:
            discardAtmosphereFeature();
            return;
        case LevelRenderFeatureKind::GlobalIllumination:
            discardGlobalIlluminationFeature();
            return;
        case LevelRenderFeatureKind::GiDenoiser:
            discardGiDenoiserFeature();
            return;
        default:
            return;
        }
    }

    void LevelRenderer::Impl::synchronizeRenderConfiguration(const RenderSettings& settings) {
        bool useRayTracing = false;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ != nullptr && hybridGi_->sharcEnabled != requestedSharcEnabled_) {
            renderPipeline_->discardFrame();
            context_.waitIdle();
            frameGraph_.reset();
            createHybridGiResources();
            pendingFrameChanges_.add(core::HistoryReason::FeatureConfigurationChanged);
        }
        const world::RenderWorldSnapshotPtr snapshot = renderWorld_.snapshot();
        useRayTracing = settings.globalIllumination.mode == GlobalIlluminationMode::RayTracing &&
                        hybridGi_ != nullptr && context_.rayTracingDecision().enabled() &&
                        context_.rayTracingSupport().supportsSharcShaderStorage() && snapshot != nullptr &&
                        !snapshot->instances().empty() && !snapshot->meshes().empty();
#else
        static_cast<void>(settings);
#endif
        const DeferredRenderPath requestedPath =
            useRayTracing ? DeferredRenderPath::Hybrid : DeferredRenderPath::Raster;
        if (renderPipeline_ == nullptr || renderPipeline_->path() != requestedPath) {
            if (renderPipeline_ != nullptr) {
                renderPipeline_->discardFrame();
            }
            createRenderFeaturePipeline(requestedPath);
            pendingFrameChanges_.add(core::HistoryReason::FeatureConfigurationChanged);
        }
    }

    LevelRenderer::Impl::FeatureConfigurationState
    LevelRenderer::Impl::featureConfiguration(const RenderSettings& settings) noexcept {
        return FeatureConfigurationState{
            .globalIlluminationMode = settings.globalIllumination.mode,
            .directLightingEnabled = settings.directLighting.enabled,
            .shadowsEnabled = settings.shadows.enabled,
            .shadowSplitLambda = settings.shadows.splitLambda,
            .shadowMaxDistance = settings.shadows.maxDistance,
            .ssaoEnabled = settings.globalIllumination.ssaoEnabled,
            .ambientOcclusionMode = settings.globalIllumination.ambientOcclusionMode,
            .ambientOcclusionRadius = settings.globalIllumination.ambientOcclusionRadius,
            .ambientOcclusionStrength = settings.globalIllumination.ambientOcclusionStrength,
            .ambientOcclusionBias = settings.globalIllumination.ambientOcclusionBias,
            .sharcEnabled = settings.globalIllumination.sharcEnabled,
            .nrdEnabled = settings.globalIllumination.nrdEnabled,
            .temporalAaEnabled = settings.temporalAa.enabled,
            .atmosphereEnabled = settings.atmosphere.enabled,
            .aerialPerspectiveEnabled = settings.atmosphere.aerialPerspective,
        };
    }

    bool LevelRenderer::Impl::shouldUseHybridGi(const RenderSettings& settings,
                                                const world::RenderWorldSnapshot& renderWorld) const noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr || renderPipeline_ == nullptr ||
            renderPipeline_->path() != DeferredRenderPath::Hybrid ||
            settings.globalIllumination.mode != GlobalIlluminationMode::RayTracing || renderWorld.instances().empty() ||
            renderWorld.meshes().empty() || renderWorld.meshes().size() > hybridGi_->geometryDescriptorCapacity) {
            return false;
        }
        return context_.rayTracingDecision().enabled() && context_.rayTracingSupport().supportsSharcShaderStorage();
#else
        static_cast<void>(settings);
        static_cast<void>(renderWorld);
        return false;
#endif
    }

} // namespace lumin::render

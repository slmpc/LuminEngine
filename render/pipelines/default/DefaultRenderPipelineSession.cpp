#include "render/pipelines/default/DefaultRenderPipelineSession.hpp"
#include "render/level/FeatureFrameData.hpp"

#include "render/gi/legacy/LegacyBackend.hpp"
#include "render/gpu/GpuScene.hpp"
#include "render/platform/vulkan/VulkanContext.hpp"

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
    pipelines::DefaultRenderPipelineSession::DefaultRenderPipelineSession(
        VulkanContext& context, world::RenderWorldSnapshotPtr initialWorld, std::filesystem::path shaderDirectory,
        ImFontAtlas& uiFontAtlas, std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination)
        : context_(context), shaderDirectory_(std::move(shaderDirectory)), uiFontAtlas_(&uiFontAtlas),
          rasterResources_(*context.rhiDevice().Get(), frameSlotCount),
          postFxResources_(*context.rhiDevice().Get(), frameSlotCount), resourceFactory_(*context.rhiDevice().Get()),
          shaderLibrary_(*context.rhiDevice().Get(), shaderDirectory_), pipelineFactory_(*context.rhiDevice().Get()),
          fullscreenPipelineFactory_(*context.rhiDevice().Get(), shaderLibrary_),
          globalIllumination_(std::move(globalIllumination)), currentWorld_(std::move(initialWorld)) {
        if (currentWorld_ == nullptr) {
            throw std::invalid_argument("Default pipeline session requires a non-empty initial render-world snapshot.");
        }
        if (globalIllumination_ == nullptr) {
            globalIllumination_ = gi::makeLegacyBackend();
        }
        pipelines::registerDefaultRenderSettings(settingsSchemas_);
        renderExtent_ = core::RenderExtent{context_.swapchainWidth(), context_.swapchainHeight()};
        requestedRenderExtent_ = renderExtent_;
        createRenderResources();
        // 先创建 RT 资源，再根据真实的 device/scene capability 选择固定的帧图拓扑。
        createRenderFeaturePipeline();
        presentation_.initialize(context_, *uiFontAtlas_, shaderLibrary_);
        presentation_.setViewportTexture(viewportOutput_.texture);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    pipelines::DefaultRenderPipelineSession::~DefaultRenderPipelineSession() {
        waitIdle();
        presentation_.shutdown();
        destroyRenderResources();
    }

    bool pipelines::DefaultRenderPipelineSession::drawFrame(core::RenderFramePacket packet, const ImDrawData& ui) {
        if (!packet.isValid()) {
            throw std::invalid_argument("Default pipeline session received an invalid render frame packet.");
        }
        currentUiDrawData_ = &ui;
        const RenderSettings settings = pipelines::readDefaultRenderSettings(packet.settings);
        if (committedSettings_.has_value()) {
            const core::FeatureSettingsChange settingsChange =
                settingsSchemas_.diff(*committedSettings_, packet.settings);
            if (!settingsChange.historyReasons.empty()) {
                pendingFrameChanges_.merge(settingsChange.historyReasons);
            }
        }
        context_.updateSurfaceState(packet.surface);
        if (swapchainGeneration_ != context_.swapchainGeneration()) {
            refreshSwapchainResources();
        }
        requestViewportExtent(packet.surface.viewportExtent.width, packet.surface.viewportExtent.height);
        applyPendingViewportExtent();
        requestedSharcEnabled_ = settings.globalIllumination.sharcEnabled;
        const world::SceneChangeMask sceneChanges = world::changesBetween(committedWorld_, packet.world);
        currentWorld_ = packet.world;
        pendingFrameChanges_.merge(core::frameChangesFromScene(sceneChanges));
        const world::SceneChangeMask rebuildChanges = world::SceneChangeMask::Geometry |
                                                      world::SceneChangeMask::InstanceTopology |
                                                      world::SceneChangeMask::MaterialBinding;
        if (world::hasAnyChange(sceneChanges, rebuildChanges)) {
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
            throw std::overflow_error("Default pipeline session exhausted the logical render sequence range.");
        }

        std::optional<VulkanFrame> frame;
        try {
            frame = context_.beginFrame();
        } catch (...) {
            throw;
        }
        if (!frame.has_value()) {
            refreshSwapchainResources();
            return false;
        }

        const core::RenderFrameIdentity identity{
            core::FrameSlotIndex{frame->frameIndex},
            core::SwapImageIndex{frame->imageIndex},
            core::RenderSequence{nextRenderSequence_},
            renderExtent_,
        };
        RecordedFrameState recorded;
        try {
            recorded =
                recordCommandList(*frame->commandList, identity, packet, settings, sceneChanges, pendingFrameChanges_);
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
        committedSettings_ = packet.settings;
        committedWorld_ = packet.world;
        hasSubmittedFrame_ = true;
        lastSubmittedFrameUsedHybridGi_ = recorded.usedHybridGlobalIllumination;
        pendingFrameChanges_.clear();
        ++nextRenderSequence_;

        const bool recreate = context_.presentFrame(*frame);
        if (recreate) {
            refreshSwapchainResources();
        }
        return true;
    }

    void pipelines::DefaultRenderPipelineSession::waitIdle() const {
        context_.waitIdle();
    }

    void pipelines::DefaultRenderPipelineSession::requestViewportExtent(std::uint32_t width,
                                                                        std::uint32_t height) noexcept {
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

    std::uint32_t pipelines::DefaultRenderPipelineSession::modelCount() const noexcept {
        return modelRenderer_ == nullptr ? 0 : modelRenderer_->drawCount();
    }

    std::uint32_t pipelines::DefaultRenderPipelineSession::mdiDrawCount() const noexcept {
        return modelCount();
    }

    gi::BackendInfo pipelines::DefaultRenderPipelineSession::globalIlluminationBackendInfo() const noexcept {
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

    const std::string& pipelines::DefaultRenderPipelineSession::diagnostic() const noexcept {
        return diagnostic_;
    }

    runtime::RenderPipelineSessionStatus pipelines::DefaultRenderPipelineSession::status() const {
        const gi::BackendInfo backend = globalIlluminationBackendInfo();
        return runtime::RenderPipelineSessionStatus{
            .modelCount = modelCount(),
            .mdiDrawCount = mdiDrawCount(),
            .globalIlluminationBackend = std::string{backend.name},
            .globalIlluminationTemporal = backend.temporal,
            .hardwareRayTracing = backend.hardwareRayTracing,
            .viewportTextureId = PresentationRenderer::viewportTextureId(),
            .viewportWidth = viewportOutput_.width,
            .viewportHeight = viewportOutput_.height,
            .diagnostic = diagnostic_,
        };
    }

    void pipelines::DefaultRenderPipelineSession::createRenderFeaturePipeline(
        pipelines::DefaultRenderPipelineKind requestedPath) {
        core::RenderDeviceCapabilities capabilities;
        capabilities.supported = {core::RenderCapability::Graphics, core::RenderCapability::Compute,
                                  core::RenderCapability::DynamicRendering};
        capabilities.maxFramesInFlight = frameSlotCount;
        pipelines::DefaultRenderPipelineKind path = pipelines::DefaultRenderPipelineKind::Raster;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        const world::RenderWorldSnapshotPtr snapshot = currentWorld_;
        const bool hybridPath = requestedPath == pipelines::DefaultRenderPipelineKind::Hybrid && hybridGi_ != nullptr &&
                                context_.rayTracingDecision().enabled() &&
                                context_.rayTracingSupport().supportsSharcShaderStorage() && snapshot != nullptr &&
                                !snapshot->instances().empty() && !snapshot->meshes().empty();
        if (hybridPath) {
            path = pipelines::DefaultRenderPipelineKind::Hybrid;
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
        const pipelines::DefaultRenderPipelineDefinition definition = pipelines::makeDefaultRenderPipeline(path);
        core::RenderFeatureRegistry registry = createFeatureRegistry(definition, path);

        const core::ResolvedRenderPipeline resolved =
            core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), capabilities);
        auto candidate = std::make_unique<core::RenderPipelineInstance>(registry, resolved,
                                                                        core::FeatureCreateContext{
                                                                            .device = context_.rhiDevice(),
                                                                            .resources = &resourceFactory_,
                                                                            .shaders = &shaderLibrary_,
                                                                            .pipelines = &pipelineFactory_,
                                                                            .capabilities = capabilities,
                                                                            .frameSlotCount = frameSlotCount,
                                                                        });
        candidate->onRenderExtentChanged(renderExtent_);
        activePipelineKind_ = path;
        renderPipeline_ = std::move(candidate);
    }

    void pipelines::DefaultRenderPipelineSession::synchronizeRenderConfiguration(const RenderSettings& settings) {
        bool useRayTracing = false;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ != nullptr && hybridGi_->sharcEnabled != requestedSharcEnabled_) {
            renderPipeline_->discardFrame();
            context_.waitIdle();
            frameGraph_.reset();
            try {
                createHybridGiResources();
                pendingFrameChanges_.add(core::HistoryReason::FeatureConfigurationChanged);
                diagnostic_.clear();
            } catch (const std::exception& exception) {
                diagnostic_ = std::string{"Hybrid resource reconfiguration failed; retaining the previous state: "} +
                              exception.what();
            }
        }
        const world::RenderWorldSnapshotPtr snapshot = currentWorld_;
        useRayTracing = settings.globalIllumination.mode == GlobalIlluminationMode::RayTracing &&
                        hybridGi_ != nullptr && context_.rayTracingDecision().enabled() &&
                        context_.rayTracingSupport().supportsSharcShaderStorage() && snapshot != nullptr &&
                        !snapshot->instances().empty() && !snapshot->meshes().empty();
#else
        static_cast<void>(settings);
#endif
        if (settings.globalIllumination.mode == GlobalIlluminationMode::RayTracing && !useRayTracing &&
            diagnostic_.empty()) {
            diagnostic_ = "Hybrid rendering is unavailable for the current device or scene; using Raster fallback.";
        } else if (settings.globalIllumination.mode != GlobalIlluminationMode::RayTracing) {
            diagnostic_.clear();
        }
        const pipelines::DefaultRenderPipelineKind requestedPath =
            useRayTracing ? pipelines::DefaultRenderPipelineKind::Hybrid : pipelines::DefaultRenderPipelineKind::Raster;
        if (renderPipeline_ == nullptr || activePipelineKind_ != requestedPath) {
            context_.waitIdle();
            try {
                // candidate 完成 DAG 解析、Feature 初始化和 extent 通知后才替换旧 PipelineInstance。
                createRenderFeaturePipeline(requestedPath);
                pendingFrameChanges_.add(core::HistoryReason::FeatureConfigurationChanged);
                if (requestedPath == pipelines::DefaultRenderPipelineKind::Hybrid) {
                    diagnostic_.clear();
                }
            } catch (const std::exception& exception) {
                diagnostic_ = std::string{"Render pipeline recomposition failed; retaining the previous recipe: "} +
                              exception.what();
            }
        }
    }

    pipelines::DefaultRenderPipelineSession::FeatureConfigurationState
    pipelines::DefaultRenderPipelineSession::featureConfiguration(const RenderSettings& settings) noexcept {
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

    bool pipelines::DefaultRenderPipelineSession::shouldUseHybridGi(
        const GlobalIlluminationSettings& settings, const world::RenderWorldSnapshot& renderWorld) const noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr || renderPipeline_ == nullptr ||
            activePipelineKind_ != pipelines::DefaultRenderPipelineKind::Hybrid ||
            settings.mode != GlobalIlluminationMode::RayTracing || renderWorld.instances().empty() ||
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

    namespace pipelines {
        namespace {

            class DefaultRenderPipelineSessionFactory final : public runtime::IRenderPipelineSessionFactory {
            public:
                [[nodiscard]] std::unique_ptr<runtime::IRenderPipelineSession>
                create(runtime::RenderPipelineSessionCreateContext context) const override {
                    if (context.vulkan == nullptr || context.initialWorld == nullptr ||
                        context.uiFontAtlas == nullptr) {
                        throw std::invalid_argument("Default pipeline session requires Vulkan, world and UI inputs.");
                    }
                    return std::make_unique<DefaultRenderPipelineSession>(
                        *context.vulkan, std::move(context.initialWorld), std::move(context.shaderDirectory),
                        *context.uiFontAtlas);
                }
            };

        } // namespace

        std::unique_ptr<runtime::IRenderPipelineSessionFactory> makeDefaultRenderPipelineSessionFactory() {
            return std::make_unique<DefaultRenderPipelineSessionFactory>();
        }
    } // namespace pipelines

} // namespace lumin::render

#include "render/level/FeatureFrameData.hpp"
#include "render/level/LevelRendererImpl.hpp"

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
#include <functional>
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
    namespace {

        struct BoundRenderFeatureCallbacks {
            using AddPasses = std::function<void(core::RenderFeatureFrameContext&)>;
            using FrameEvent = std::function<void(const core::RenderFrameIdentity&)>;

            BoundRenderFeatureCallbacks(AddPasses addPassesCallback, FrameEvent submittedCallback = {},
                                        FrameEvent discardedCallback = {})
                : addPasses(std::move(addPassesCallback)), submitted(std::move(submittedCallback)),
                  discarded(std::move(discardedCallback)) {
            }

            AddPasses addPasses;
            FrameEvent submitted;
            FrameEvent discarded;
        };

        // 迁移期 Feature 对象直接绑定单一职责回调；注册点显式列出模块，不再通过枚举和中央 switch 分派。
        class BoundRenderFeature final : public core::IRenderFeature {
        public:
            BoundRenderFeature(core::FeatureDescriptor descriptor, BoundRenderFeatureCallbacks callbacks)
                : descriptor_(std::move(descriptor)), callbacks_(std::move(callbacks)) {
            }

            [[nodiscard]] const core::FeatureDescriptor& descriptor() const noexcept override {
                return descriptor_;
            }

            void addPasses(core::RenderFeatureFrameContext& context) override {
                callbacks_.addPasses(context);
            }

            void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
                if (callbacks_.submitted) {
                    callbacks_.submitted(identity);
                }
            }

            void onFrameDiscarded(const core::RenderFrameIdentity& identity) noexcept override {
                if (callbacks_.discarded) {
                    callbacks_.discarded(identity);
                }
            }

        private:
            core::FeatureDescriptor descriptor_;
            BoundRenderFeatureCallbacks callbacks_;
        };

    } // namespace

    LevelRenderer::LevelRenderer(VulkanContext& context, world::RenderWorldSnapshotPtr initialWorld,
                                 std::filesystem::path shaderDirectory, core::UiFontAtlas uiFontAtlas,
                                 std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination)
        : impl_(std::make_unique<Impl>(context, std::move(initialWorld), std::move(shaderDirectory),
                                       std::move(uiFontAtlas), std::move(globalIllumination))) {
    }

    LevelRenderer::~LevelRenderer() = default;

    void LevelRenderer::drawFrame(core::RenderFramePacket packet) {
        impl_->drawFrame(std::move(packet));
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

    ImGuiViewportImage LevelRenderer::viewportImage() const noexcept {
        return impl_->viewportImage();
    }

    LevelRenderer::Impl::Impl(VulkanContext& context, world::RenderWorldSnapshotPtr initialWorld,
                              std::filesystem::path shaderDirectory, core::UiFontAtlas uiFontAtlas,
                              std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination)
        : context_(context), shaderDirectory_(std::move(shaderDirectory)), uiFontAtlas_(std::move(uiFontAtlas)),
          rasterResources_(*context.rhiDevice().Get(), frameSlotCount),
          postFxResources_(*context.rhiDevice().Get(), frameSlotCount),
          fullscreenPipelineFactory_(*context.rhiDevice().Get(), shaderDirectory_),
          globalIllumination_(std::move(globalIllumination)), currentWorld_(std::move(initialWorld)) {
        if (currentWorld_ == nullptr) {
            throw std::invalid_argument("LevelRenderer requires a non-empty initial render-world snapshot.");
        }
        if (globalIllumination_ == nullptr) {
            globalIllumination_ = gi::makeLegacyBackend(shaderDirectory_);
        }
        renderExtent_ = core::RenderExtent{context_.swapchainWidth(), context_.swapchainHeight()};
        requestedRenderExtent_ = renderExtent_;
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

    void LevelRenderer::Impl::drawFrame(core::RenderFramePacket packet) {
        if (!packet.isValid()) {
            throw std::invalid_argument("LevelRenderer received an invalid render frame packet.");
        }
        const RenderSettings settings = pipelines::readDefaultRenderSettings(packet.settings);
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
        committedWorld_ = packet.world;
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

    void LevelRenderer::Impl::createRenderFeaturePipeline(pipelines::DefaultRenderPipelineKind requestedPath) {
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
        core::RenderFeatureRegistry registry;
        const auto registerFeature = [&registry, &definition](const core::FeatureId& id,
                                                              BoundRenderFeatureCallbacks callbacks) {
            core::FeatureDescriptor descriptor = definition.descriptor(id);
            registry.registerFeature(descriptor, [descriptor = std::move(descriptor), callbacks = std::move(callbacks)](
                                                     const core::FeatureCreateContext&) mutable {
                return std::make_unique<BoundRenderFeature>(descriptor, callbacks);
            });
        };

        using namespace pipelines::feature_ids;
        registerFeature(pipelines::feature_ids::atmosphere(), {[this](core::RenderFeatureFrameContext& context) {
                                                                   addAtmosphereLutFeaturePasses(context);
                                                               },
                                                               [this](const core::RenderFrameIdentity& identity) {
                                                                   commitAtmosphereFeature(identity);
                                                               },
                                                               [this](const core::RenderFrameIdentity&) {
                                                                   discardAtmosphereFeature();
                                                               }});
        if (path == pipelines::DefaultRenderPipelineKind::Raster) {
            registerFeature(shadow(), {[this](core::RenderFeatureFrameContext& context) {
                                addShadowFeaturePasses(context);
                            }});
            registerFeature(rasterSurface(), {[this](core::RenderFeatureFrameContext& context) {
                                                  addGBufferFeaturePasses(context);
                                              },
                                              [this](const core::RenderFrameIdentity&) {
                                                  if (modelRenderer_ != nullptr) {
                                                      modelRenderer_->commitSubmittedFrame();
                                                  }
                                              },
                                              [this](const core::RenderFrameIdentity&) {
                                                  if (modelRenderer_ != nullptr) {
                                                      modelRenderer_->discardPendingFrame();
                                                  }
                                              }});
        } else {
            registerFeature(hybridSurface(), {[this](core::RenderFeatureFrameContext& context) {
                                                  addHybridSurfaceFeaturePasses(context);
                                              },
                                              [this](const core::RenderFrameIdentity& identity) {
                                                  commitHybridSurfaceFeature(identity);
                                                  if (modelRenderer_ != nullptr) {
                                                      modelRenderer_->commitSubmittedFrame();
                                                  }
                                              },
                                              [this](const core::RenderFrameIdentity&) {
                                                  discardHybridSurfaceFeature();
                                                  if (modelRenderer_ != nullptr) {
                                                      modelRenderer_->discardPendingFrame();
                                                  }
                                              }});
        }
        registerFeature(globalIllumination(), {[this](core::RenderFeatureFrameContext& context) {
                                                   addGlobalIlluminationFeaturePasses(context);
                                               },
                                               [this](const core::RenderFrameIdentity& identity) {
                                                   commitGlobalIlluminationFeature(identity);
                                               },
                                               [this](const core::RenderFrameIdentity&) {
                                                   discardGlobalIlluminationFeature();
                                               }});
        registerFeature(denoising(), {[this](core::RenderFeatureFrameContext& context) {
                                          addGiDenoiserFeaturePasses(context);
                                      },
                                      [this](const core::RenderFrameIdentity& identity) {
                                          commitGiDenoiserFeature(identity);
                                      },
                                      [this](const core::RenderFrameIdentity&) {
                                          discardGiDenoiserFeature();
                                      }});
        registerFeature(lightingComposite(), {[this](core::RenderFeatureFrameContext& context) {
                            addSkyCompositeFeaturePasses(context);
                            addDirectLightingFeaturePasses(context);
                        }});
        registerFeature(temporalAa(), {[this](core::RenderFeatureFrameContext& context) {
                                           addTemporalAaFeaturePasses(context);
                                       },
                                       [this](const core::RenderFrameIdentity& identity) {
                                           postFxResources_.markHistoryValid(identity.frameSlot.value());
                                       }});
        registerFeature(toneMapping(), {[this](core::RenderFeatureFrameContext& context) {
                            addToneMappingFeaturePasses(context);
                        }});
        registerFeature(presentation(), {[this](core::RenderFeatureFrameContext& context) {
                                             addUiPresentFeaturePasses(context);
                                         },
                                         [this](const core::RenderFrameIdentity&) {
                                             viewportOutputInitialized_ = true;
                                         }});

        const core::ResolvedRenderPipeline resolved =
            core::RenderPipelineRecipeResolver::resolve(registry, definition.recipe(), capabilities);
        auto candidate = std::make_unique<core::RenderPipelineInstance>(
            registry, resolved,
            core::FeatureCreateContext{
                .device = context_.rhiDevice(), .capabilities = capabilities, .frameSlotCount = frameSlotCount});
        candidate->onRenderExtentChanged(renderExtent_);
        activePipelineKind_ = path;
        renderPipeline_ = std::move(candidate);
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
        const world::RenderWorldSnapshotPtr snapshot = currentWorld_;
        useRayTracing = settings.globalIllumination.mode == GlobalIlluminationMode::RayTracing &&
                        hybridGi_ != nullptr && context_.rayTracingDecision().enabled() &&
                        context_.rayTracingSupport().supportsSharcShaderStorage() && snapshot != nullptr &&
                        !snapshot->instances().empty() && !snapshot->meshes().empty();
#else
        static_cast<void>(settings);
#endif
        const pipelines::DefaultRenderPipelineKind requestedPath =
            useRayTracing ? pipelines::DefaultRenderPipelineKind::Hybrid : pipelines::DefaultRenderPipelineKind::Raster;
        if (renderPipeline_ == nullptr || activePipelineKind_ != requestedPath) {
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

    bool LevelRenderer::Impl::shouldUseHybridGi(const GlobalIlluminationSettings& settings,
                                                const world::RenderWorldSnapshot& renderWorld) const noexcept {
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

} // namespace lumin::render

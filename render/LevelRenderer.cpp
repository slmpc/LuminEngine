#include "render/LevelRenderer.hpp"

#include "render/VulkanContext.hpp"
#include "render/gi/SsaoBackend.hpp"
#include "render/gpu/GpuScene.hpp"
#include "render/platform/Window.hpp"
#include "scene/Camera.hpp"
#include "scene/Level.hpp"

#if defined(LUMIN_HAS_NRD) && LUMIN_HAS_NRD && defined(LUMIN_HAS_SHARC) && LUMIN_HAS_SHARC
#define LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI 1
#include "render/gi/GiComposite.hpp"
#include "render/gi/HybridLightingComposite.hpp"
#include "render/gi/NrdDenoiser.hpp"
#include "render/gi/RayTracedDirectLighting.hpp"
#include "render/gi/RayTracedGi.hpp"
#include "render/gi/SharcRadianceCache.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#else
#define LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI 0
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
#include <glm/gtc/type_ptr.hpp>
#include <glm/trigonometric.hpp>

namespace lumin::render {
    namespace {

        constexpr float cascadeSplitLambda = 0.68f;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        constexpr float atmosphereLuxToRendererRadiance = 1.0e-5f;
        constexpr float nrdDenoisingRange = 500000.0f;
#endif

        struct CascadeShadowData {
            std::array<glm::mat4, shadowCascadeCount> viewProjections{};
            glm::vec4 splits{0.0f};
        };

        struct DeferredFrameData {
            world::RenderWorldSnapshotPtr renderWorldSnapshot;
            const world::RenderWorldSnapshot* renderWorld = nullptr;
            const scene::Camera* camera = nullptr;
            const RenderSettings* settings = nullptr;
            const TextureFrameResources* frame = nullptr;
            std::uint32_t frameIndex = 0;
            std::uint32_t imageIndex = 0;
            std::uint32_t historyReadIndex = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            world::SceneChangeMask sceneChanges = world::SceneChangeMask::None;
            glm::mat4 view{1.0f};
            glm::mat4 projection{1.0f};
            glm::mat4 viewProjection{1.0f};
            glm::mat4 previousViewProjection{1.0f};
            glm::vec2 jitter{0.0f};
            PostProcessUniforms uniforms;
            CascadeShadowData cascades;
            std::array<FrameGraphResourceHandle, shadowCascadeCount> shadows{};
            FrameGraphResourceHandle position;
            FrameGraphResourceHandle normal;
            FrameGraphResourceHandle albedo;
            FrameGraphResourceHandle motion;
            FrameGraphResourceHandle materialId;
            FrameGraphResourceHandle materials;
            FrameGraphResourceHandle depth;
            FrameGraphResourceHandle globalIllumination;
            FrameGraphResourceHandle lighting;
            /// TAA 读取的最终 HDR 输入；Raster 为 lighting，Hybrid 为 direct+indirect composite。
            FrameGraphResourceHandle taaInput;
            FrameGraphResourceHandle taaResolved;
            FrameGraphResourceHandle historyRead;
            FrameGraphResourceHandle historyWrite;
            FrameGraphResourceHandle swap;
            FrameGraphResourceHandle imguiFont;
            std::optional<atmosphere::AtmosphereLutGraphRecord> atmosphereLuts;
            std::array<nvrhi::FramebufferHandle, shadowCascadeCount> shadowFramebuffers{};
            nvrhi::FramebufferHandle gbufferFramebuffer;
            nvrhi::FramebufferHandle lightingFramebuffer;
            nvrhi::FramebufferHandle taaFramebuffer;
            nvrhi::FramebufferHandle tonemapFramebuffer;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
            bool hybridGiActive = false;
            gpu::GpuSceneDescriptors hybridSceneDescriptors;
            std::span<const gpu::GpuGeometryDescriptor> hybridGeometry;
            FrameGraphResourceHandle hybridTlas;
            FrameGraphResourceHandle hybridInstances;
            FrameGraphResourceHandle hybridMaterials;
            FrameGraphPassHandle hybridSceneReadyPass;
            std::vector<FrameGraphResourceHandle> hybridVertices;
            std::vector<FrameGraphResourceHandle> hybridIndices;
            std::vector<FrameGraphResourceHandle> hybridBaseColorTextures;
            std::vector<FrameGraphResourceHandle> hybridNormalRoughnessTextures;
            std::optional<gi::SharcGraphRecord> sharcRecord;
            std::optional<gi::RayTracedGiGraphSignals> rayTracedSignals;
            gi::RtSurfaceSignalGraphResources hybridSurface;
            FrameGraphPassHandle hybridSurfacePass;
#endif
            bool hybridPathActive = false;
        };

        glm::vec3 normalizedLightDirection(glm::vec3 direction) {
            if (glm::dot(direction, direction) < 1e-6f) {
                direction = glm::vec3{-0.45f, -0.8f, -0.35f};
            }
            return glm::normalize(direction);
        }

        float halton(std::uint32_t index, std::uint32_t base) {
            float result = 0.0f;
            float fraction = 1.0f;
            while (index > 0) {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(index % base);
                index /= base;
            }
            return result;
        }

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        std::array<float, 16> matrixElements(const glm::mat4& matrix) {
            std::array<float, 16> result{};
            std::copy_n(glm::value_ptr(matrix), result.size(), result.begin());
            return result;
        }

        glm::vec4 rendererSunRadiance(const scene::DirectionalLight& sun, bool directLightingEnabled) {
            const float directScale =
                directLightingEnabled ? sun.illuminanceLux * atmosphereLuxToRendererRadiance : 0.0f;
            // w 控制 RT miss 与 SHARC fallback 的环境可见性；关闭物理大气时仍需保留程序化天空。
            return glm::vec4{sun.color * directScale, 1.0f};
        }
#endif

        CascadeShadowData calculateCascadeShadows(const scene::Camera& camera, float aspectRatio,
                                                  glm::vec3 lightDirection) {
            CascadeShadowData result;
            std::array<float, shadowCascadeCount> splits{};
            const float clipRange = camera.farPlane() - camera.nearPlane();
            const float clipRatio = camera.farPlane() / camera.nearPlane();
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(shadowCascadeCount);
                const float logarithmic = camera.nearPlane() * std::pow(clipRatio, fraction);
                const float uniform = camera.nearPlane() + clipRange * fraction;
                splits[cascade] = cascadeSplitLambda * logarithmic + (1.0f - cascadeSplitLambda) * uniform;
                result.splits[cascade] = splits[cascade];
            }

            lightDirection = normalizedLightDirection(lightDirection);
            const glm::vec3 cameraForward = camera.forward();
            const glm::vec3 cameraRight = camera.right();
            const glm::vec3 cameraUp = camera.up();
            const float tanHalfFov = std::tan(glm::radians(camera.fieldOfViewDegrees()) * 0.5f);
            const glm::vec3 upReference = std::abs(glm::dot(lightDirection, glm::vec3{0.0f, 1.0f, 0.0f})) > 0.95f
                                              ? glm::vec3{0.0f, 0.0f, 1.0f}
                                              : glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::vec3 lightRight = glm::normalize(glm::cross(lightDirection, upReference));
            const glm::vec3 lightUp = glm::normalize(glm::cross(lightRight, lightDirection));

            float sliceNear = camera.nearPlane();
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                const float sliceFar = splits[cascade];
                const float nearHalfHeight = tanHalfFov * sliceNear;
                const float nearHalfWidth = nearHalfHeight * aspectRatio;
                const float farHalfHeight = tanHalfFov * sliceFar;
                const float farHalfWidth = farHalfHeight * aspectRatio;
                const glm::vec3 nearCenter = camera.position() + cameraForward * sliceNear;
                const glm::vec3 farCenter = camera.position() + cameraForward * sliceFar;
                const std::array<glm::vec3, 8> corners = {
                    nearCenter - cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,
                    nearCenter + cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,
                    nearCenter + cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
                    nearCenter - cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
                    farCenter - cameraRight * farHalfWidth - cameraUp * farHalfHeight,
                    farCenter + cameraRight * farHalfWidth - cameraUp * farHalfHeight,
                    farCenter + cameraRight * farHalfWidth + cameraUp * farHalfHeight,
                    farCenter - cameraRight * farHalfWidth + cameraUp * farHalfHeight,
                };

                glm::vec3 center{0.0f};
                for (const glm::vec3& corner : corners) {
                    center += corner;
                }
                center /= static_cast<float>(corners.size());

                float radius = 0.0f;
                for (const glm::vec3& corner : corners) {
                    radius = std::max(radius, glm::length(corner - center));
                }
                radius = std::ceil(radius * 16.0f) / 16.0f;
                const float texelSize = (2.0f * radius) / static_cast<float>(shadowMapResolution);
                const float centerRight = glm::dot(center, lightRight);
                const float centerUp = glm::dot(center, lightUp);
                const float snappedRight = std::floor(centerRight / texelSize + 0.5f) * texelSize;
                const float snappedUp = std::floor(centerUp / texelSize + 0.5f) * texelSize;
                const glm::vec3 snappedCenter =
                    center + lightRight * (snappedRight - centerRight) + lightUp * (snappedUp - centerUp);

                const float casterMargin = std::max(25.0f, radius * 0.5f);
                const float lightDistance = radius + casterMargin;
                const glm::mat4 lightView =
                    glm::lookAt(snappedCenter - lightDirection * lightDistance, snappedCenter, lightUp);
                glm::mat4 lightProjection =
                    glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.1f, 2.0f * (radius + casterMargin));
                result.viewProjections[cascade] = lightProjection * lightView;
                sliceNear = sliceFar;
            }
            return result;
        }

        FrameGraphTextureDesc textureDesc(const GpuTexture& image,
                                          nvrhi::ResourceStates initialState = nvrhi::ResourceStates::Unknown) {
            FrameGraphTextureDesc desc;
            desc.texture = image.texture;
            desc.initialState = initialState;
            // 每个交换帧资源在本帧末尾都以 shader-readable 状态交给下一帧；深度附件由调用方覆盖为 DepthWrite。
            desc.finalState = nvrhi::ResourceStates::ShaderResource;
            return desc;
        }

        nvrhi::FramebufferHandle createFramebuffer(nvrhi::IDevice& device, const nvrhi::FramebufferDesc& desc) {
            nvrhi::FramebufferHandle framebuffer = device.createFramebuffer(desc);
            if (!framebuffer) {
                throw std::runtime_error("Failed to create an NvRHI LevelRenderer framebuffer.");
            }
            return framebuffer;
        }

    } // namespace

    struct LevelRenderer::HybridGiState {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
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

    LevelRenderer::LevelRenderer(platform::Window& window, VulkanContext& context, const scene::Level& level,
                                 std::filesystem::path shaderDirectory,
                                 std::unique_ptr<gi::GlobalIlluminationBackend> globalIllumination)
        : window_(window), context_(context), level_(level), shaderDirectory_(std::move(shaderDirectory)),
          textures_(context), pipelines_(context, shaderDirectory_),
          globalIllumination_(std::move(globalIllumination)) {
        if (globalIllumination_ == nullptr) {
            globalIllumination_ = gi::makeSsaoBackend(shaderDirectory_);
        }
        static_cast<void>(renderWorld_.sync(level_));
        createRenderResources();
        // 先创建 RT 资源，再根据真实的 device/scene capability 选择固定的帧图拓扑。
        createRenderFeaturePipeline();
        imgui_.initialize(window_, context_);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    LevelRenderer::~LevelRenderer() {
        waitIdle();
        imgui_.shutdown();
        destroyRenderResources();
    }

    void LevelRenderer::beginUiFrame(ImGuiContent* content) {
        if (swapchainGeneration_ != context_.swapchainGeneration()) {
            refreshSwapchainResources();
        }
        imgui_.beginFrame(content);
    }

    void LevelRenderer::cancelUiFrame() noexcept {
        imgui_.cancelFrame();
    }

    void LevelRenderer::drawFrame(scene::Camera& camera, RenderSettings& settings, ImGuiContent* content) {
        if (swapchainGeneration_ != context_.swapchainGeneration()) {
            refreshSwapchainResources();
        }
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

        const FeatureConfigurationState currentConfiguration = featureConfiguration(settings);
        if (hasSubmittedFrame_ && currentConfiguration != committedFeatureConfiguration_) {
            pendingFrameChanges_.add(core::HistoryReason::FeatureConfigurationChanged);
        }

        if (!imgui_.framePrepared()) {
            imgui_.beginFrame(content);
        }

        if (nextRenderSequence_ == core::RenderSequence::invalidValue) {
            imgui_.cancelFrame();
            throw std::overflow_error("LevelRenderer exhausted the logical render sequence range.");
        }

        std::optional<VulkanFrame> frame;
        try {
            frame = context_.beginFrame();
        } catch (...) {
            imgui_.cancelFrame();
            throw;
        }
        if (!frame.has_value()) {
            imgui_.cancelFrame();
            refreshSwapchainResources();
            return;
        }

        const core::RenderFrameIdentity identity{
            core::FrameSlotIndex{frame->frameIndex},
            core::SwapImageIndex{frame->imageIndex},
            core::RenderSequence{nextRenderSequence_},
            core::RenderExtent{context_.swapchainWidth(), context_.swapchainHeight()},
        };
        RecordedFrameState recorded;
        try {
            recorded = recordCommandList(*frame->commandList, identity, camera, settings, sceneDelta.changes,
                                         pendingFrameChanges_);
        } catch (...) {
            const std::exception_ptr recordingFailure = std::current_exception();
            discardPendingRuntimeFrame();
            renderPipeline_->discardFrame();
            imgui_.cancelFrame();
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
            discardPendingRuntimeFrame();
            renderPipeline_->discardFrame();
            imgui_.cancelFrame();
            throw;
        }

        commitSubmittedRuntimeFrame(identity);
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

    void LevelRenderer::waitIdle() const {
        context_.waitIdle();
    }

    ImGuiCaptureState LevelRenderer::imguiCaptureState() const noexcept {
        return imgui_.captureState();
    }

    std::uint32_t LevelRenderer::modelCount() const noexcept {
        return modelRenderer_ == nullptr ? 0 : modelRenderer_->drawCount();
    }

    std::uint32_t LevelRenderer::mdiDrawCount() const noexcept {
        return modelCount();
    }

    gi::BackendInfo LevelRenderer::globalIlluminationBackendInfo() const noexcept {
        if (lastSubmittedFrameUsedHybridGi_) {
            return gi::BackendInfo{"Ray Tracing + SHARC + NRD", true, true};
        }
        return globalIllumination_->info();
    }

    void LevelRenderer::createRenderFeaturePipeline() {
        DeferredRenderPipelineCallbacks callbacks;
        callbacks.shadow.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addShadowFeaturePasses(context);
        };
        callbacks.gbuffer.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addGBufferFeaturePasses(context);
        };
        callbacks.gbuffer.onSubmitted = [this](const core::RenderFrameIdentity&) {
            if (modelRenderer_ != nullptr) {
                modelRenderer_->commitSubmittedFrame();
            }
        };
        callbacks.gbuffer.onDiscarded = [this](const core::RenderFrameIdentity&) {
            if (modelRenderer_ != nullptr) {
                modelRenderer_->discardPendingFrame();
            }
        };
        callbacks.hybridSurface.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addHybridSurfaceFeaturePasses(context);
        };
        callbacks.hybridSurface.onSubmitted = [this](const core::RenderFrameIdentity&) {
            if (modelRenderer_ != nullptr) {
                modelRenderer_->commitSubmittedFrame();
            }
        };
        callbacks.hybridSurface.onDiscarded = [this](const core::RenderFrameIdentity&) {
            if (modelRenderer_ != nullptr) {
                modelRenderer_->discardPendingFrame();
            }
        };
        callbacks.atmosphereLuts.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addAtmosphereLutFeaturePasses(context);
        };
        callbacks.globalIllumination.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addGlobalIlluminationFeaturePasses(context);
        };
        callbacks.giDenoiser.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addGiDenoiserFeaturePasses(context);
        };
        callbacks.skyComposite.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addSkyCompositeFeaturePasses(context);
        };
        callbacks.directLighting.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addDirectLightingFeaturePasses(context);
        };
        callbacks.temporalAa.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addTemporalAaFeaturePasses(context);
        };
        callbacks.temporalAa.onSubmitted = [this](const core::RenderFrameIdentity& identity) {
            textures_.markHistoryValid(identity.frameSlot.value());
        };
        callbacks.toneMapping.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addToneMappingFeaturePasses(context);
        };
        callbacks.uiPresent.addPasses = [this](core::RenderFeatureFrameContext& context) {
            addUiPresentFeaturePasses(context);
        };
        callbacks.uiPresent.onSubmitted = [this](const core::RenderFrameIdentity&) {
            imgui_.markFontTextureInitialized();
        };

        core::RenderDeviceCapabilities capabilities;
        capabilities.supported = {core::RenderCapability::Graphics, core::RenderCapability::Compute,
                                  core::RenderCapability::DynamicRendering};
        capabilities.maxFramesInFlight = TextureManager::maxFramesInFlight;
        DeferredRenderPath path = DeferredRenderPath::Raster;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        const world::RenderWorldSnapshotPtr snapshot = renderWorld_.snapshot();
        const bool hybridPath = hybridGi_ != nullptr && context_.rayTracingDecision().enabled() &&
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
#endif
        renderPipeline_ = std::make_unique<DeferredRenderPipeline>(std::move(callbacks), capabilities, path);
    }

    void LevelRenderer::createRenderResources() {
        const std::uint32_t width = context_.swapchainWidth();
        const std::uint32_t height = context_.swapchainHeight();
        textures_.create(width, height);
        createDirectLightingBindingLayout();
        atmosphereLutGpu_ = std::make_unique<atmosphere::AtmosphereLutGpu>(atmosphere::AtmosphereLutGpuCreateInfo{
            .device = context_.rhiDevice(),
            .shaderDirectory = shaderDirectory_,
            .frameSlotCount = TextureManager::maxFramesInFlight,
            .quality = {},
        });
        createAtmosphereConsumerBindings();
        pipelines_.create(textures_.bindingLayout(), directLightingBindingLayout_, atmosphereConsumerBindingLayout_,
                          textures_.lightingFormat(), context_.swapchainRhiFormat());

        std::array<gi::FrameResources, TextureManager::maxFramesInFlight> giFrames{};
        for (std::uint32_t frameIndex = 0; frameIndex < giFrames.size(); ++frameIndex) {
            const TextureFrameResources& frame = textures_.frame(frameIndex);
            giFrames[frameIndex].position = frame.position.texture;
            giFrames[frameIndex].normalRoughness = frame.normalRoughness.texture;
            giFrames[frameIndex].albedoMetallic = frame.albedo.texture;
            giFrames[frameIndex].motion = frame.motion.texture;
            giFrames[frameIndex].depth = frame.depth.texture;
            giFrames[frameIndex].uniformBuffer = frame.postProcessUniform.buffer;
            giFrames[frameIndex].output = frame.globalIllumination.texture;
        }
        globalIllumination_->create(gi::CreateInfo{context_.rhiDevice(),
                                                   {width, height},
                                                   textures_.globalIlluminationFormat(),
                                                   textures_.sampler(),
                                                   giFrames});
        atmosphereForceRebuild_ = true;
        createModelRenderer();
        createHybridGiResources();
        frameResourcesInitialized_.fill(false);
    }

    void LevelRenderer::createModelRenderer() {
        const world::RenderWorldSnapshotPtr snapshot = renderWorld_.snapshot();
        if (snapshot == nullptr) {
            throw std::logic_error("LevelRenderer requires a synchronized render-world snapshot.");
        }
        if (snapshot->instances().empty()) {
            modelRenderer_.reset();
            return;
        }
        const std::array<nvrhi::Format, 5> colorFormats = {textures_.positionFormat(), textures_.normalFormat(),
                                                           textures_.albedoFormat(), textures_.motionFormat(),
                                                           textures_.materialIdFormat()};
        modelRenderer_ = std::make_unique<ModelRenderer>(
            context_, *snapshot, shaderDirectory_, colorFormats, textures_.depthFormat(), textures_.shadowDepthFormat(),
            TextureManager::maxFramesInFlight, context_.modelRendererCapabilities());
        createDirectLightingBindingSets();
    }

    void LevelRenderer::createDirectLightingBindingLayout() {
        nvrhi::VulkanBindingOffsets offsets;
        offsets.setShaderResourceOffset(0).setSamplerOffset(0).setConstantBufferOffset(0).setUnorderedAccessViewOffset(
            0);

        nvrhi::BindingLayoutDesc desc;
        desc.setVisibility(nvrhi::ShaderType::Pixel)
            .setRegisterSpaceAndDescriptorSet(1)
            .setBindingOffsets(offsets)
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        directLightingBindingLayout_ = context_.rhiDevice()->createBindingLayout(desc);
        if (!directLightingBindingLayout_) {
            throw std::runtime_error("Failed to create the direct-lighting material binding layout.");
        }
    }

    void LevelRenderer::createDirectLightingBindingSets() {
        directLightingBindingSets_.fill(nullptr);
        if (modelRenderer_ == nullptr) {
            return;
        }
        for (std::uint32_t frameIndex = 0; frameIndex < directLightingBindingSets_.size(); ++frameIndex) {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, textures_.frame(frameIndex).materialId.texture))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, modelRenderer_->materialBuffer(frameIndex)));
            directLightingBindingSets_[frameIndex] =
                context_.rhiDevice()->createBindingSet(desc, directLightingBindingLayout_);
            if (!directLightingBindingSets_[frameIndex]) {
                directLightingBindingSets_.fill(nullptr);
                throw std::runtime_error("Failed to create a direct-lighting material binding set.");
            }
        }
    }

    void LevelRenderer::createAtmosphereConsumerBindings() {
        atmosphereConsumerBindingSets_.fill(nullptr);
        atmosphereConsumerBindingLayout_ =
            context_.rhiDevice()->createBindingLayout(atmosphere::detail::makeAtmosphereConsumerBindingLayoutDesc());
        if (!atmosphereConsumerBindingLayout_) {
            throw std::runtime_error("Failed to create the shared atmosphere consumer binding layout.");
        }
        for (std::uint32_t frameIndex = 0; frameIndex < atmosphereConsumerBindingSets_.size(); ++frameIndex) {
            atmosphereConsumerBindingSets_[frameIndex] = context_.rhiDevice()->createBindingSet(
                atmosphere::detail::makeAtmosphereConsumerBindingSetDesc(atmosphereLutGpu_->constantBuffer(frameIndex),
                                                                         atmosphereLutGpu_->nativeResources()),
                atmosphereConsumerBindingLayout_);
            if (!atmosphereConsumerBindingSets_[frameIndex]) {
                atmosphereConsumerBindingSets_.fill(nullptr);
                atmosphereConsumerBindingLayout_ = nullptr;
                throw std::runtime_error("Failed to create a shared atmosphere consumer binding set.");
            }
        }
    }

    void LevelRenderer::createHybridGiResources() {
        destroyHybridGiResources();
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (!context_.rayTracingDecision().enabled() || !context_.rayTracingSupport().supportsSharcShaderStorage()) {
            return;
        }
        const world::RenderWorldSnapshotPtr snapshot = renderWorld_.snapshot();
        if (snapshot == nullptr) {
            throw std::logic_error("Hybrid GI requires a synchronized render-world snapshot.");
        }
        if (modelRenderer_ == nullptr || modelRenderer_->baseColorTextures().empty() ||
            modelRenderer_->baseColorTextures().size() > std::numeric_limits<std::uint16_t>::max()) {
            return;
        }
        const std::size_t requestedCapacity = std::max<std::size_t>(snapshot->meshes().size(), 1U);
        if (requestedCapacity > std::numeric_limits<std::uint16_t>::max()) {
            // NvRHI descriptor-array size is uint16_t；超大场景保留 SSAO 回退，不创建部分可用的 RT runtime。
            return;
        }

        auto runtime = std::make_unique<HybridGiState>();
        runtime->geometryDescriptorCapacity = static_cast<std::uint32_t>(requestedCapacity);
        const std::uint32_t materialTextureDescriptorCapacity =
            static_cast<std::uint32_t>(modelRenderer_->baseColorTextures().size());
        runtime->sceneBackend = std::make_unique<gpu::NvrhiGpuSceneBackend>(*context_.rhiDevice());
        runtime->sceneResources = std::make_unique<gpu::GpuSceneResources>(
            *runtime->sceneBackend, gpu::GpuSceneResourceConfig{TextureManager::maxFramesInFlight, true});
        runtime->scenePlanner = std::make_unique<gpu::GpuSceneUpdatePlanner>();

        for (std::uint32_t frameIndex = 0; frameIndex < TextureManager::maxFramesInFlight; ++frameIndex) {
            const TextureFrameResources& frame = textures_.frame(frameIndex);
            runtime->directLightingFrames[frameIndex] = gi::RayTracedDiFrameResources{
                .worldPositionHitT = frame.position.texture,
                .normalRoughness = frame.normalRoughness.texture,
                .albedoMetallic = frame.albedo.texture,
                .materialId = frame.materialId.texture,
                .viewZ = {},
                .motion = frame.motion.texture,
                // Hybrid RTDI 使用 globalIllumination 作为 direct-radiance UAV，最终合成再写 lighting。
                .directRadiance = frame.globalIllumination.texture,
                .visibilityMask = {},
            };
        }
        runtime->directLighting =
            std::make_unique<gi::RayTracedDirectLightingPass>(gi::RayTracedDirectLightingPass::CreateInfo{
                .device = context_.rhiDevice(),
                .shaderDirectory = shaderDirectory_,
                .width = context_.swapchainWidth(),
                .height = context_.swapchainHeight(),
                .maxGeometryDescriptors = runtime->geometryDescriptorCapacity,
                .maxMaterialTextureDescriptors = materialTextureDescriptorCapacity,
                .atmosphereBindingLayout = atmosphereConsumerBindingLayout_,
                .frames = runtime->directLightingFrames,
            });

        std::array<gi::RayTracedGiFrameInputs, TextureManager::maxFramesInFlight> rayTracedFrames{};
        std::array<gi::SharcUpdateFrameInputs, TextureManager::maxFramesInFlight> sharcFrames{};
        for (std::uint32_t frameIndex = 0; frameIndex < TextureManager::maxFramesInFlight; ++frameIndex) {
            const TextureFrameResources& frame = textures_.frame(frameIndex);
            rayTracedFrames[frameIndex] = gi::RayTracedGiFrameInputs{
                .position = frame.position.texture,
                .normalRoughness = frame.normalRoughness.texture,
                .albedoMetallic = frame.albedo.texture,
                .motion = frame.motion.texture,
            };
            sharcFrames[frameIndex] = gi::SharcUpdateFrameInputs{
                .position = frame.position.texture,
                .normalRoughness = frame.normalRoughness.texture,
                .albedoMetallic = frame.albedo.texture,
            };
        }

        runtime->sharc = std::make_unique<gi::SharcRadianceCache>(gi::SharcRadianceCacheCreateInfo{
            .device = context_.rhiDevice(),
            .shaderDirectory = shaderDirectory_,
            .frameSlotCount = TextureManager::maxFramesInFlight,
            .maxGeometryDescriptors = runtime->geometryDescriptorCapacity,
            .maxMaterialTextureDescriptors = materialTextureDescriptorCapacity,
            .atmosphereBindingLayout = atmosphereConsumerBindingLayout_,
            .config = {},
            .frames = sharcFrames,
        });
        runtime->rayTracedGi = std::make_unique<gi::RayTracedGiPass>(gi::RayTracedGiCreateInfo{
            .device = context_.rhiDevice(),
            .shaderDirectory = shaderDirectory_,
            .width = context_.swapchainWidth(),
            .height = context_.swapchainHeight(),
            .maxGeometryDescriptors = runtime->geometryDescriptorCapacity,
            .maxMaterialTextureDescriptors = materialTextureDescriptorCapacity,
            .atmosphereBindingLayout = atmosphereConsumerBindingLayout_,
            .enableSharc = true,
            .frames = rayTracedFrames,
        });
        runtime->nrd = std::make_unique<gi::NrdDenoiser>(gi::NrdDenoiserCreateInfo{
            .device = context_.rhiDevice(),
            .extent = core::RenderExtent{context_.swapchainWidth(), context_.swapchainHeight()},
            .frameSlotCount = TextureManager::maxFramesInFlight,
        });
        if (runtime->rayTracedGi->formats().normalRoughness != runtime->nrd->expectedNormalRoughnessFormat()) {
            throw std::runtime_error("RT GI and NRD normal/roughness formats do not match.");
        }
        runtime->composite = std::make_unique<gi::GiCompositePass>(gi::GiCompositeCreateInfo{
            .device = context_.rhiDevice(),
            .shaderDirectory = shaderDirectory_,
            .extent = core::RenderExtent{context_.swapchainWidth(), context_.swapchainHeight()},
            .frameSlotCount = TextureManager::maxFramesInFlight,
        });
        runtime->lightingComposite =
            std::make_unique<gi::HybridLightingCompositePass>(gi::HybridLightingCompositeCreateInfo{
                .device = context_.rhiDevice(),
                .shaderDirectory = shaderDirectory_,
                .extent = core::RenderExtent{context_.swapchainWidth(), context_.swapchainHeight()},
                .frameSlotCount = TextureManager::maxFramesInFlight,
            });
        hybridGi_ = std::move(runtime);
#endif
    }

    void LevelRenderer::ensureHybridGiCapacity() {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (!context_.rayTracingDecision().enabled() || !context_.rayTracingSupport().supportsSharcShaderStorage()) {
            destroyHybridGiResources();
            return;
        }
        const world::RenderWorldSnapshotPtr snapshot = renderWorld_.snapshot();
        if (snapshot == nullptr) {
            throw std::logic_error("Hybrid GI capacity check requires a render-world snapshot.");
        }
        const std::size_t requiredCapacity = std::max<std::size_t>(snapshot->meshes().size(), 1U);
        if (requiredCapacity > std::numeric_limits<std::uint16_t>::max()) {
            destroyHybridGiResources();
            return;
        }
        if (hybridGi_ == nullptr || requiredCapacity > hybridGi_->geometryDescriptorCapacity) {
            createHybridGiResources();
        }
#endif
    }

    void LevelRenderer::destroyDirectLightingBindings() noexcept {
        directLightingBindingSets_.fill(nullptr);
        directLightingBindingLayout_ = nullptr;
    }

    void LevelRenderer::destroyHybridGiResources() noexcept {
        hybridGi_.reset();
        lastSubmittedFrameUsedHybridGi_ = false;
    }

    void LevelRenderer::destroyRenderResources() noexcept {
        discardPendingRuntimeFrame();
        frameGraph_.reset();
        destroyHybridGiResources();
        directLightingBindingSets_.fill(nullptr);
        modelRenderer_.reset();
        pipelines_.destroy();
        atmosphereConsumerBindingSets_.fill(nullptr);
        atmosphereConsumerBindingLayout_ = nullptr;
        atmosphereLutGpu_.reset();
        atmosphereLutScheduler_ = {};
        atmosphereForceRebuild_ = true;
        globalIllumination_->destroy();
        destroyDirectLightingBindings();
        textures_.destroy();
    }

    void LevelRenderer::refreshSwapchainResources() {
        discardPendingRuntimeFrame();
        renderPipeline_->discardFrame();
        pendingFrameChanges_.add(core::HistoryReason::SwapchainRecreated | core::HistoryReason::RenderExtentChanged);
        context_.waitIdle();
        imgui_.shutdown();
        destroyRenderResources();
        createRenderResources();
        imgui_.initialize(window_, context_);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

    void LevelRenderer::commitSubmittedRuntimeFrame(const core::RenderFrameIdentity& identity) {
        if (pendingAtmosphereSequence_) {
            if (*pendingAtmosphereSequence_ != identity.sequence || atmosphereLutGpu_ == nullptr ||
                !atmosphereLutGpu_->hasPendingFrame() || !atmosphereLutScheduler_.hasActiveFrame()) {
                throw std::logic_error("Atmosphere LUT transaction does not match the submitted render frame.");
            }
            atmosphereLutGpu_->commitSubmittedFrame();
            atmosphereLutScheduler_.commitSubmittedFrame(identity.sequence);
            pendingAtmosphereSequence_.reset();
            atmosphereForceRebuild_ = false;
        }

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ != nullptr && hybridGi_->pendingSequence) {
            HybridGiState& runtime = *hybridGi_;
            if (*runtime.pendingSequence != identity.sequence || !runtime.pendingScenePlan ||
                !runtime.pendingSceneUpdate || !runtime.directLighting->hasPendingFrame() ||
                runtime.pendingSceneUpdate->frameSlot() != identity.frameSlot) {
                throw std::logic_error("Hybrid GI transaction does not match the submitted render frame.");
            }

            const bool giPending = runtime.pendingNrdFrame.has_value() || runtime.sharc->hasPendingFrame() ||
                                   runtime.rayTracedGi->hasPendingFrame() || runtime.nrd->hasPendingFrame();
            if (giPending &&
                (!runtime.pendingNrdFrame || runtime.pendingNrdFrame->sequence() != identity.sequence ||
                 runtime.pendingNrdFrame->frameSlot() != identity.frameSlot || !runtime.sharc->hasPendingFrame() ||
                 !runtime.rayTracedGi->hasPendingFrame() || !runtime.nrd->hasPendingFrame())) {
                throw std::logic_error("Hybrid GI denoiser transaction does not match the submitted render frame.");
            }

            runtime.scenePlanner->commit(*runtime.pendingScenePlan,
                                         gpu::GpuSceneCommitInfo{identity.frameSlot, true, true, true});
            runtime.sceneResources->finishUpdate(*runtime.pendingSceneUpdate, true);
            runtime.directLighting->commitSubmittedFrame();
            if (giPending) {
                runtime.sharc->commitSubmittedFrame();
                runtime.rayTracedGi->commitSubmittedFrame();
                runtime.nrd->commitSubmittedFrame(*runtime.pendingNrdFrame);
            }
            runtime.pendingNrdFrame.reset();
            runtime.pendingSceneUpdate.reset();
            runtime.pendingScenePlan.reset();
            runtime.pendingSequence.reset();
        }
#endif
    }

    void LevelRenderer::discardPendingRuntimeFrame() noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ != nullptr) {
            HybridGiState& runtime = *hybridGi_;
            if (runtime.pendingNrdFrame) {
                runtime.nrd->discardFrame(*runtime.pendingNrdFrame);
            } else {
                runtime.nrd->discardPendingFrame();
            }
            runtime.directLighting->discardPendingFrame();
            runtime.rayTracedGi->discardPendingFrame();
            runtime.sharc->discardPendingFrame();
            if (runtime.pendingSceneUpdate) {
                try {
                    runtime.sceneResources->finishUpdate(*runtime.pendingSceneUpdate, false);
                } catch (...) {
                    // discard 路径不得以第二个异常覆盖原始录制或提交错误。
                }
            }
            runtime.pendingNrdFrame.reset();
            runtime.pendingSceneUpdate.reset();
            runtime.pendingScenePlan.reset();
            runtime.pendingSequence.reset();
        }
#endif
        if (atmosphereLutGpu_ != nullptr) {
            atmosphereLutGpu_->discardPendingFrame();
        }
        if (pendingAtmosphereSequence_ && atmosphereLutScheduler_.hasActiveFrame()) {
            try {
                atmosphereLutScheduler_.abandonFrame(*pendingAtmosphereSequence_);
            } catch (...) {
                // discard 路径不得以第二个异常覆盖原始录制或提交错误。
            }
        }
        pendingAtmosphereSequence_.reset();
    }

    LevelRenderer::FeatureConfigurationState
    LevelRenderer::featureConfiguration(const RenderSettings& settings) noexcept {
        return FeatureConfigurationState{
            .globalIlluminationMode = settings.globalIllumination.mode,
            .directLightingEnabled = settings.directLighting.enabled,
            .shadowsEnabled = settings.shadows.enabled,
            .globalIlluminationEnabled = settings.globalIllumination.enabled,
            .temporalAaEnabled = settings.temporalAa.enabled,
            .atmosphereEnabled = settings.atmosphere.enabled,
            .aerialPerspectiveEnabled = settings.atmosphere.aerialPerspective,
        };
    }

    bool LevelRenderer::shouldUseHybridGi(const RenderSettings& settings,
                                          const world::RenderWorldSnapshot& renderWorld) const noexcept {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (hybridGi_ == nullptr || !settings.globalIllumination.enabled ||
            settings.globalIllumination.mode == GlobalIlluminationMode::Ssao || renderWorld.instances().empty() ||
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

    LevelRenderer::RecordedFrameState
    LevelRenderer::recordCommandList(nvrhi::ICommandList& commandList, const core::RenderFrameIdentity& identity,
                                     const scene::Camera& camera, const RenderSettings& settings,
                                     world::SceneChangeMask sceneChanges, const core::FrameChangeSet& changes) {
        if (!identity.isValid()) {
            throw std::invalid_argument("LevelRenderer requires a valid render frame identity.");
        }
        const std::uint32_t frameIndex = identity.frameSlot.value();
        const std::uint32_t imageIndex = identity.swapImage.value();
        const std::uint32_t width = identity.extent.width;
        const std::uint32_t height = identity.extent.height;
        const float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
        const glm::mat4 view = camera.viewMatrix();
        const glm::mat4 unjitteredProjection = camera.projectionMatrix(aspectRatio);
        const glm::mat4 unjitteredViewProjection = unjitteredProjection * view;
        glm::mat4 projection = unjitteredProjection;
        glm::vec2 jitter{0.0f};
        if (settings.temporalAa.enabled) {
            const std::uint32_t jitterIndex = static_cast<std::uint32_t>(identity.sequence.value() % 8U) + 1U;
            jitter = glm::vec2{halton(jitterIndex, 2U) - 0.5f, halton(jitterIndex, 3U) - 0.5f};
            projection[2][0] += (2.0f * jitter.x) / static_cast<float>(width);
            projection[2][1] += (2.0f * jitter.y) / static_cast<float>(height);
        }
        const glm::mat4 viewProjection = projection * view;
        const world::RenderWorldSnapshotPtr renderWorld = renderWorld_.snapshot();
        if (renderWorld == nullptr) {
            throw std::logic_error("LevelRenderer cannot record without a render-world snapshot.");
        }

        const scene::DirectionalLight& sun = renderWorld->environment().sun;
        const glm::vec3 cameraForward = camera.forward();
        const glm::vec3 lightDirection = normalizedLightDirection(sun.direction);
        const CascadeShadowData cascades = calculateCascadeShadows(camera, aspectRatio, lightDirection);
        const std::uint32_t historyReadIndex =
            (frameIndex + TextureManager::maxFramesInFlight - 1) % TextureManager::maxFramesInFlight;
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        const TextureFrameResources& historyReadFrame = textures_.frame(historyReadIndex);

        DeferredFrameData data;
        data.renderWorldSnapshot = renderWorld;
        data.renderWorld = renderWorld.get();
        data.camera = &camera;
        data.settings = &settings;
        data.frame = &frame;
        data.frameIndex = frameIndex;
        data.imageIndex = imageIndex;
        data.historyReadIndex = historyReadIndex;
        data.width = width;
        data.height = height;
        data.sceneChanges = sceneChanges;
        data.view = view;
        data.projection = unjitteredProjection;
        data.viewProjection = viewProjection;
        data.previousViewProjection = previousViewProjection_;
        data.jitter = jitter;
        data.cascades = cascades;
        data.hybridPathActive = renderPipeline_ != nullptr && renderPipeline_->path() == DeferredRenderPath::Hybrid;
        data.uniforms.inverseViewProjection = glm::inverse(unjitteredViewProjection);
        data.uniforms.viewProjection = viewProjection;
        data.uniforms.cascadeViewProjections = cascades.viewProjections;
        data.uniforms.cascadeSplits = cascades.splits;
        data.uniforms.cameraPosition = glm::vec4{camera.position(), 1.0f};
        data.uniforms.cameraForward = glm::vec4{cameraForward, 0.0f};
        data.uniforms.lightDirection = glm::vec4{lightDirection, settings.directLighting.enabled ? 1.0f : 0.0f};
        data.uniforms.renderSize = glm::vec4{static_cast<float>(width), static_cast<float>(height),
                                             1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
        data.uniforms.renderOptions = glm::vec4{0.0f, settings.globalIllumination.enabled ? 1.0f : 0.0f,
                                                settings.shadows.enabled && sun.castsShadows ? 1.0f : 0.0f,
                                                settings.temporalAa.enabled ? 1.0f : 0.0f};
        data.uniforms.tonemapOptions.x = settings.toneMapping.exposure;
        data.uniforms.tonemapOptions.y = context_.swapchainIsSrgb() ? 1.0f : 0.0f;

        frameGraph_.reset();
        const nvrhi::ResourceStates frameInitialState = frameResourcesInitialized_[frameIndex]
                                                            ? nvrhi::ResourceStates::ShaderResource
                                                            : nvrhi::ResourceStates::Common;
        const nvrhi::ResourceStates depthInitialState =
            frameResourcesInitialized_[frameIndex] ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::Common;
        if (!data.hybridPathActive) {
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                data.shadows[cascade] =
                    frameGraph_.importTexture("shadow.cascade" + std::to_string(cascade),
                                              textureDesc(frame.shadowCascades[cascade], frameInitialState));
            }
            data.position =
                frameGraph_.importTexture("gbuffer.position", textureDesc(frame.position, frameInitialState));
            data.normal =
                frameGraph_.importTexture("gbuffer.normal", textureDesc(frame.normalRoughness, frameInitialState));
            data.albedo = frameGraph_.importTexture("gbuffer.albedo", textureDesc(frame.albedo, frameInitialState));
            data.motion = frameGraph_.importTexture("gbuffer.motion", textureDesc(frame.motion, frameInitialState));
            FrameGraphTextureDesc materialIdDesc = textureDesc(frame.materialId, frameInitialState);
            materialIdDesc.finalState = nvrhi::ResourceStates::ShaderResource;
            data.materialId = frameGraph_.importTexture("gbuffer.material-id", materialIdDesc);
            if (modelRenderer_ != nullptr) {
                const nvrhi::BufferHandle& materialBuffer = modelRenderer_->materialBuffer(frameIndex);
                data.materials = frameGraph_.importBuffer(
                    "gpu-scene.materials",
                    FrameGraphBufferDesc{.size = materialBuffer->getDesc().byteSize,
                                         .buffer = materialBuffer,
                                         .initialState = modelRenderer_->materialBufferInitialState(frameIndex),
                                         .finalState = nvrhi::ResourceStates::ShaderResource});
            }
            FrameGraphTextureDesc depthDesc = textureDesc(frame.depth, depthInitialState);
            depthDesc.finalState = nvrhi::ResourceStates::DepthWrite;
            data.depth = frameGraph_.importTexture("gbuffer.depth", depthDesc);
        }
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        else {
            if (hybridGi_ == nullptr || hybridGi_->directLighting == nullptr) {
                throw std::logic_error("Hybrid render topology requires the RT surface runtime.");
            }
            const gi::RayTracedDiFrameResources& surface = hybridGi_->directLighting->signals(frameIndex);
            data.position = frameGraph_.importTexture(
                "rt.surface.world-position", textureDesc(GpuTexture{surface.worldPositionHitT}, frameInitialState));
            data.normal = frameGraph_.importTexture(
                "rt.surface.normal-roughness", textureDesc(GpuTexture{surface.normalRoughness}, frameInitialState));
            data.albedo = frameGraph_.importTexture("rt.surface.albedo-metallic",
                                                    textureDesc(GpuTexture{surface.albedoMetallic}, frameInitialState));
            data.materialId = frameGraph_.importTexture("rt.surface.material-id",
                                                        textureDesc(GpuTexture{surface.materialId}, frameInitialState));
            data.motion = frameGraph_.importTexture("rt.surface.motion",
                                                    textureDesc(GpuTexture{surface.motion}, frameInitialState));
            data.depth = frameGraph_.importTexture("rt.surface.view-z",
                                                   textureDesc(GpuTexture{surface.viewZ}, frameInitialState));
            data.hybridSurface.worldPositionHitT = data.position;
            data.hybridSurface.normalRoughness = data.normal;
            data.hybridSurface.albedoMetallic = data.albedo;
            data.hybridSurface.materialId = data.materialId;
            data.hybridSurface.viewZ = data.depth;
            data.hybridSurface.motion = data.motion;
            data.hybridSurface.visibilityMask = frameGraph_.importTexture(
                "rt.surface.visibility", textureDesc(GpuTexture{surface.visibilityMask}, frameInitialState));
        }
#endif
        data.globalIllumination = frameGraph_.importTexture("global-illumination.output",
                                                            textureDesc(frame.globalIllumination, frameInitialState));
        data.lighting = frameGraph_.importTexture(data.hybridPathActive ? "rt.surface.direct-radiance" : "lighting.hdr",
                                                  textureDesc(frame.lighting, frameInitialState));
        data.taaInput = data.lighting;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (data.hybridPathActive) {
            data.hybridSurface.directRadiance = data.globalIllumination;
        }
#endif
        data.taaResolved = frameGraph_.importTexture("taa.resolved", textureDesc(frame.taaResolved, frameInitialState));
        data.historyRead = frameGraph_.importTexture(
            "taa.history.read", textureDesc(historyReadFrame.history, textures_.historyInitialState(historyReadIndex)));
        data.historyWrite = frameGraph_.importTexture(
            "taa.history.write", textureDesc(frame.history, textures_.historyInitialState(frameIndex)));

        FrameGraphTextureDesc swapDesc;
        swapDesc.texture = context_.swapchainTextures().at(imageIndex);
        swapDesc.initialState = context_.swapchainTextureInitialState(imageIndex);
        if (swapDesc.initialState == nvrhi::ResourceStates::Unknown) {
            swapDesc.initialState = nvrhi::ResourceStates::Common;
        }
        swapDesc.finalState = nvrhi::ResourceStates::Present;
        data.swap = frameGraph_.importTexture("swapchain.color", swapDesc);
        FrameGraphTextureDesc fontDesc;
        fontDesc.texture = imgui_.fontTexture();
        fontDesc.initialState = imgui_.fontTextureInitialState();
        fontDesc.finalState = nvrhi::ResourceStates::ShaderResource;
        data.imguiFont = frameGraph_.importTexture("imgui.font", fontDesc);

        nvrhi::IDevice& device = *context_.rhiDevice();
        if (!data.hybridPathActive) {
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                data.shadowFramebuffers[cascade] = createFramebuffer(
                    device, nvrhi::FramebufferDesc().setDepthAttachment(frame.shadowCascades[cascade].texture));
            }
            nvrhi::FramebufferDesc gbufferDesc;
            gbufferDesc.addColorAttachment(frame.position.texture)
                .addColorAttachment(frame.normalRoughness.texture)
                .addColorAttachment(frame.albedo.texture)
                .addColorAttachment(frame.motion.texture)
                .addColorAttachment(frame.materialId.texture)
                .setDepthAttachment(frame.depth.texture);
            data.gbufferFramebuffer = createFramebuffer(device, gbufferDesc);
            data.lightingFramebuffer =
                createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(frame.lighting.texture));
        }
        data.taaFramebuffer =
            createFramebuffer(device, nvrhi::FramebufferDesc().addColorAttachment(frame.taaResolved.texture));
        data.tonemapFramebuffer = createFramebuffer(
            device, nvrhi::FramebufferDesc().addColorAttachment(context_.swapchainTextures().at(imageIndex)));

        core::RenderBlackboard blackboard;
        blackboard.set(std::move(data));
        blackboard.set(AtmosphereInvalidationSignatures{
            .optical = renderWorld->sourceAtmosphereRevision(),
            .lighting = renderWorld->sourceLightingRevision(),
            .view = camera.revision(),
        });
        renderPipeline_->prepareFrame(identity, camera.cutEpoch(), changes, frameGraph_, blackboard);
        frameGraph_.execute(FrameGraphContext{&commandList, nullptr, frameIndex});
        bool usedHybridGlobalIllumination = false;
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        usedHybridGlobalIllumination = blackboard.get<DeferredFrameData>().hybridGiActive;
#endif
        return RecordedFrameState{viewProjection,
                                  view,
                                  unjitteredProjection,
                                  jitter,
                                  featureConfiguration(settings),
                                  usedHybridGlobalIllumination};
    }

    void LevelRenderer::addShadowFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        FrameGraph& graph = context.frameGraph();
        for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            graph.addPass(
                "CSM clear " + std::to_string(cascade), FrameGraphPassType::Transfer,
                [shadow = data.shadows[cascade]](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::CopyDest);
                },
                [texture = data.frame->shadowCascades[cascade].texture](const FrameGraphContext& frameContext) {
                    frameContext.commandList->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, 1.0f,
                                                                       false, 0);
                });
            graph.addPass(
                "CSM cascade " + std::to_string(cascade), FrameGraphPassType::Graphics,
                [shadow = data.shadows[cascade]](FrameGraphBuilder& builder) {
                    builder.writeTexture(shadow, nvrhi::ResourceStates::DepthWrite);
                },
                [this, &data, cascade,
                 framebuffer = data.shadowFramebuffers[cascade]](const FrameGraphContext& frameContext) {
                    recordShadowPass(*frameContext.commandList, *framebuffer, data.frameIndex, cascade,
                                     data.cascades.viewProjections[cascade]);
                });
        }
    }

    void LevelRenderer::addGBufferFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        FrameGraph& graph = context.frameGraph();
        const bool resetMotion = context.historyAction(core::HistoryDomain::Taa) == core::HistoryAction::FullReset;
        data.previousViewProjection = resetMotion ? data.viewProjection : previousViewProjection_;
        if (modelRenderer_ != nullptr) {
            modelRenderer_->sync(*data.renderWorld, data.frameIndex, resetMotion);
        }
        graph.addPass(
            "G-buffer clear", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color : {data.position, data.normal, data.albedo, data.motion}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::CopyDest);
                }
                builder.writeTexture(data.materialId, nvrhi::ResourceStates::CopyDest);
                builder.writeTexture(data.depth, nvrhi::ResourceStates::CopyDest);
            },
            [&data](const FrameGraphContext& frameContext) {
                const std::array<nvrhi::Color, 4> colors = {
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}, nvrhi::Color{0.0f, 0.0f, 1.0f, 1.0f},
                    nvrhi::Color{0.0f, 0.0f, 0.0f, 1.0f}, nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f}};
                const std::array<const GpuTexture*, 4> images = {&data.frame->position, &data.frame->normalRoughness,
                                                                 &data.frame->albedo, &data.frame->motion};
                for (std::uint32_t index = 0; index < images.size(); ++index) {
                    frameContext.commandList->clearTextureFloat(images[index]->texture, nvrhi::AllSubresources,
                                                                colors[index]);
                }
                frameContext.commandList->clearTextureUInt(data.frame->materialId.texture, nvrhi::AllSubresources,
                                                           gpu::GpuMaterialIndex::invalidValue);
                frameContext.commandList->clearDepthStencilTexture(data.frame->depth.texture, nvrhi::AllSubresources,
                                                                   true, 1.0f, false, 0);
            });
        graph.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle color : {data.position, data.normal, data.albedo, data.motion}) {
                    builder.writeTexture(color, nvrhi::ResourceStates::RenderTarget);
                }
                builder.writeTexture(data.materialId, nvrhi::ResourceStates::RenderTarget);
                builder.writeTexture(data.depth, nvrhi::ResourceStates::DepthWrite);
            },
            [this, &data, framebuffer = data.gbufferFramebuffer](const FrameGraphContext& frameContext) {
                recordGBufferPass(*frameContext.commandList, *framebuffer, data.frameIndex, data.viewProjection,
                                  data.previousViewProjection);
            });
    }

    void LevelRenderer::addHybridSurfaceFeaturePasses(core::RenderFeatureFrameContext& context) {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        if (!data.hybridPathActive) {
            return;
        }
        if (hybridGi_ == nullptr || hybridGi_->directLighting == nullptr || hybridGi_->scenePlanner == nullptr ||
            hybridGi_->sceneResources == nullptr) {
            throw std::logic_error("Hybrid surface requires the RT Scene and direct-lighting runtime.");
        }
        HybridGiState& runtime = *hybridGi_;
        if (runtime.pendingSequence || runtime.pendingScenePlan || runtime.pendingSceneUpdate ||
            runtime.pendingNrdFrame || runtime.sharc->hasPendingFrame() || runtime.rayTracedGi->hasPendingFrame() ||
            runtime.nrd->hasPendingFrame() || runtime.directLighting->hasPendingFrame()) {
            throw std::logic_error("Hybrid surface already owns an unfinished render frame.");
        }
        if (!data.atmosphereLuts || data.frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[data.frameIndex]) {
            throw std::logic_error("Hybrid surface requires the current atmosphere LUT graph and binding set.");
        }

        const bool resetMotion = context.historyAction(core::HistoryDomain::Taa) == core::HistoryAction::FullReset;
        data.previousViewProjection = resetMotion ? data.viewProjection : previousViewProjection_;
        if (modelRenderer_ != nullptr) {
            modelRenderer_->sync(*data.renderWorld, data.frameIndex, resetMotion);
        }
        if (modelRenderer_ == nullptr || modelRenderer_->baseColorTextures().empty() ||
            modelRenderer_->baseColorTextures().size() != modelRenderer_->normalRoughnessTextures().size() ||
            !modelRenderer_->materialSampler()) {
            throw std::logic_error("Hybrid surface requires the shared model material table and texture descriptors.");
        }

        runtime.pendingSequence = context.identity().sequence;
        runtime.pendingScenePlan = runtime.scenePlanner->plan(world::SceneDelta{
            .changes = data.sceneChanges,
            .snapshot = data.renderWorldSnapshot,
        });
        runtime.pendingSceneUpdate = runtime.sceneResources->recordUpdate(
            context.frameGraph(), *runtime.pendingScenePlan, context.identity().frameSlot, true);

        const gpu::GpuScenePreparedUpdate& update = *runtime.pendingSceneUpdate;
        data.hybridSceneDescriptors = runtime.sceneResources->candidateDescriptors(update);
        data.hybridGeometry = runtime.sceneResources->candidateGeometry(update);
        const std::span<const gpu::GpuGeometryFrameGraphResources> geometryResources = update.geometryResources();
        if (geometryResources.size() != data.hybridGeometry.size()) {
            throw std::logic_error("GPU Scene native and FrameGraph geometry arrays do not match.");
        }
        data.hybridVertices.clear();
        data.hybridIndices.clear();
        data.hybridVertices.reserve(geometryResources.size());
        data.hybridIndices.reserve(geometryResources.size());
        for (const gpu::GpuGeometryFrameGraphResources& geometry : geometryResources) {
            data.hybridVertices.push_back(geometry.vertices);
            data.hybridIndices.push_back(geometry.indices);
        }
        data.hybridTlas = update.tlasResource();
        data.hybridInstances = update.instanceRecordsResource();
        const nvrhi::BufferHandle& materialBuffer = modelRenderer_->materialBuffer(data.frameIndex);
        data.materials = context.frameGraph().importBuffer(
            "rt.materials",
            FrameGraphBufferDesc{.size = materialBuffer->getDesc().byteSize,
                                 .buffer = materialBuffer,
                                 .initialState = modelRenderer_->materialBufferInitialState(data.frameIndex),
                                 .finalState = nvrhi::ResourceStates::ShaderResource});
        data.hybridSceneDescriptors.materials = materialBuffer;
        data.hybridMaterials = data.materials;
        data.hybridBaseColorTextures.clear();
        data.hybridNormalRoughnessTextures.clear();
        const std::span<const nvrhi::TextureHandle> baseColorTextures = modelRenderer_->baseColorTextures();
        const std::span<const nvrhi::TextureHandle> normalRoughnessTextures = modelRenderer_->normalRoughnessTextures();
        data.hybridBaseColorTextures.reserve(baseColorTextures.size());
        data.hybridNormalRoughnessTextures.reserve(normalRoughnessTextures.size());
        for (std::size_t index = 0; index < baseColorTextures.size(); ++index) {
            const auto importMaterialTexture = [&](const char* prefix, const nvrhi::TextureHandle& texture) {
                return context.frameGraph().importTexture(
                    std::string{prefix} + std::to_string(index),
                    FrameGraphTextureDesc{.texture = texture,
                                          .initialState = nvrhi::ResourceStates::ShaderResource,
                                          .finalState = nvrhi::ResourceStates::ShaderResource});
            };
            data.hybridBaseColorTextures.push_back(
                importMaterialTexture("rt.material-base-color.", baseColorTextures[index]));
            data.hybridNormalRoughnessTextures.push_back(
                importMaterialTexture("rt.material-normal-roughness.", normalRoughnessTextures[index]));
        }
        data.hybridSceneReadyPass = update.accelerationStructurePass();
        if (!data.hybridSceneReadyPass.isValid()) {
            data.hybridSceneReadyPass = update.uploadPass();
        }

        const scene::DirectionalLight& sun = data.renderWorld->environment().sun;
        const glm::vec3 toSun = -normalizedLightDirection(sun.direction);
        const glm::vec4 sunRadiance = rendererSunRadiance(sun, data.settings->directLighting.enabled);
        const gi::RayTracingEnvironmentBindings environment{
            .atmosphere = atmosphereConsumerBindingSets_[data.frameIndex],
        };
        const gi::RayTracingEnvironmentGraphResources environmentResources{
            .atmosphere = data.atmosphereLuts->resources,
        };
        const gi::RayTracedDiConstants directConstants{
            .inverseViewProjection = glm::inverse(data.viewProjection),
            .previousViewProjection = data.previousViewProjection,
            .cameraPosition = glm::vec4{data.camera->position(), 1.0f},
            .cameraForward = glm::vec4{data.camera->forward(), 0.0f},
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderSize = glm::vec4{static_cast<float>(data.width), static_cast<float>(data.height),
                                    -data.jitter.x / static_cast<float>(data.width),
                                    data.jitter.y / static_cast<float>(data.height)},
            .traceParameters =
                glm::vec4{0.001f, data.camera->farPlane(), data.settings->directLighting.enabled ? 1.0f : 0.0f,
                          static_cast<float>(context.identity().sequence.value())},
        };
        data.hybridSurfacePass = runtime.directLighting->record(
            context.frameGraph(), data.frameIndex, true, directConstants, data.hybridSurface,
            gi::RayTracedGiSceneBindings{
                .descriptors = data.hybridSceneDescriptors,
                .geometry = data.hybridGeometry,
                .baseColorTextures = baseColorTextures,
                .normalRoughnessTextures = normalRoughnessTextures,
                .materialSampler = modelRenderer_->materialSampler(),
            },
            gi::RayTracedGiSceneGraphResources{
                .tlas = data.hybridTlas,
                .instances = data.hybridInstances,
                .materials = data.hybridMaterials,
                .vertices = std::span<const FrameGraphResourceHandle>{data.hybridVertices},
                .indices = std::span<const FrameGraphResourceHandle>{data.hybridIndices},
                .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.hybridBaseColorTextures},
                .normalRoughnessTextures =
                    std::span<const FrameGraphResourceHandle>{data.hybridNormalRoughnessTextures},
                .readyPass = data.hybridSceneReadyPass,
            },
            environment, environmentResources);
#else
        static_cast<void>(context);
#endif
    }

    void LevelRenderer::addAtmosphereLutFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        if (atmosphereLutGpu_ == nullptr) {
            throw std::logic_error("Atmosphere LUT GPU resources are not initialized.");
        }
        if (pendingAtmosphereSequence_) {
            throw std::logic_error("LevelRenderer already owns a pending atmosphere LUT frame.");
        }

        scene::SceneEnvironment environment = data.renderWorld->environment();
        environment.atmosphere.enabled = environment.atmosphere.enabled && data.settings->atmosphere.enabled;
        const atmosphere::AtmosphereViewInput view =
            atmosphere::makeAtmosphereViewInput(*data.camera, context.identity().extent);
        const atmosphere::AtmosphereLutSignatures signatures =
            atmosphere::makeAtmosphereLutSignatures(environment, view);
        const atmosphere::AtmosphereLutPlan lutPlan = atmosphereLutScheduler_.beginFrame(
            atmosphere::AtmosphereLutFrameInput{context.identity().sequence, signatures, atmosphereForceRebuild_});
        pendingAtmosphereSequence_ = context.identity().sequence;
        const atmosphere::AtmosphereGpuConstants constants = atmosphere::buildAtmosphereGpuConstants(environment, view);
        data.atmosphereLuts = atmosphereLutGpu_->record(context.frameGraph(), data.frameIndex, true, constants,
                                                        atmosphere::makeAtmosphereLutPassPlan(lutPlan));
    }

    void LevelRenderer::addGlobalIlluminationFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        core::HistoryAction fallbackAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
        fallbackAction =
            core::strongerHistoryAction(fallbackAction, context.historyAction(core::HistoryDomain::NrdSpecular));
        fallbackAction = core::strongerHistoryAction(fallbackAction, context.historyAction(core::HistoryDomain::Sharc));

        if (!shouldUseHybridGi(*data.settings, *data.renderWorld)) {
            if (fallbackAction != core::HistoryAction::Keep) {
                globalIllumination_->invalidateHistory();
            }
            globalIllumination_->addPasses(context.frameGraph(), gi::FrameInfo{
                                                                     *data.renderWorld,
                                                                     data.frameIndex,
                                                                     context.identity().sequence.value(),
                                                                     data.settings->globalIllumination.enabled,
                                                                     fallbackAction == core::HistoryAction::FullReset,
                                                                     {data.width, data.height},
                                                                     data.position,
                                                                     data.normal,
                                                                     data.albedo,
                                                                     data.motion,
                                                                     data.depth,
                                                                     data.globalIllumination,
                                                                 });
            return;
        }

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        HybridGiState& runtime = *hybridGi_;
        if (!runtime.pendingSequence || *runtime.pendingSequence != context.identity().sequence ||
            !runtime.pendingScenePlan || !runtime.pendingSceneUpdate || !runtime.directLighting->hasPendingFrame() ||
            runtime.pendingNrdFrame || runtime.sharc->hasPendingFrame() || runtime.rayTracedGi->hasPendingFrame() ||
            runtime.nrd->hasPendingFrame()) {
            throw std::logic_error("Hybrid GI requires the current frame's pending surface transaction.");
        }
        if (!data.atmosphereLuts || data.frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[data.frameIndex]) {
            throw std::logic_error("Hybrid GI requires the current atmosphere LUT graph and binding set.");
        }

        const scene::DirectionalLight& sun = data.renderWorld->environment().sun;
        const glm::vec3 toSun = -normalizedLightDirection(sun.direction);
        const glm::vec4 sunRadiance = rendererSunRadiance(sun, data.settings->directLighting.enabled);
        gi::SharcInvalidationInputs sharcInvalidation{
            .cameraCut = context.changes().containsAny(core::HistoryReason::CameraCut),
            .topologyChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::InstanceTopology),
            .geometryChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::Geometry),
            .materialChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::TransformOrMaterial |
                                                                          world::SceneChangeMask::MaterialBinding),
            .lightingChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::Lighting),
            .atmosphereChanged = world::hasAnyChange(data.sceneChanges, world::SceneChangeMask::Atmosphere),
        };
        const core::HistoryAction sharcAction = context.historyAction(core::HistoryDomain::Sharc);
        if (sharcAction == core::HistoryAction::FullReset) {
            sharcInvalidation.topologyChanged = true;
        } else if (sharcAction == core::HistoryAction::SoftReset) {
            sharcInvalidation.materialChanged = true;
        }

        const gi::RayTracingEnvironmentBindings environment{
            .atmosphere = atmosphereConsumerBindingSets_[data.frameIndex],
        };
        const gi::RayTracingEnvironmentGraphResources environmentResources{
            .atmosphere = data.atmosphereLuts->resources,
        };
        const gi::SharcUpdateSceneBindings sceneBindings{
            .descriptors = data.hybridSceneDescriptors,
            .geometry = data.hybridGeometry,
            .baseColorTextures = modelRenderer_->baseColorTextures(),
            .normalRoughnessTextures = modelRenderer_->normalRoughnessTextures(),
            .materialSampler = modelRenderer_->materialSampler(),
        };
        const gi::SharcUpdateSceneGraphResources sceneGraphResources{
            .tlas = data.hybridTlas,
            .instances = data.hybridInstances,
            .materials = data.hybridMaterials,
            .vertices = std::span<const FrameGraphResourceHandle>{data.hybridVertices},
            .indices = std::span<const FrameGraphResourceHandle>{data.hybridIndices},
            .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.hybridBaseColorTextures},
            .normalRoughnessTextures = std::span<const FrameGraphResourceHandle>{data.hybridNormalRoughnessTextures},
            .readyPass = data.hybridSceneReadyPass,
        };
        const gi::SharcFrameParameters sharcFrame{
            .cameraPosition = glm::vec4{data.camera->position(), 1.0f},
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderWidth = data.width,
            .renderHeight = data.height,
            .minTraceDistance = 0.001f,
            .maxTraceDistance = data.camera->farPlane(),
        };
        data.sharcRecord = runtime.sharc->record(context.frameGraph(), data.frameIndex, true, sharcFrame,
                                                 sharcInvalidation, environment, environmentResources,
                                                 gi::SharcUpdateFrameGraphInputs{data.hybridSurface.worldPositionHitT,
                                                                                 data.hybridSurface.normalRoughness,
                                                                                 data.hybridSurface.albedoMetallic},
                                                 sceneBindings, sceneGraphResources);

        const core::HistoryAction diffuseAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
        const core::HistoryAction specularAction = context.historyAction(core::HistoryDomain::NrdSpecular);
        const bool resetNrdMotion = !hasSubmittedFrame_ || diffuseAction == core::HistoryAction::FullReset ||
                                    specularAction == core::HistoryAction::FullReset;
        const glm::vec2 previousJitter = resetNrdMotion ? data.jitter : previousJitter_;
        const glm::vec2 currentEffectiveJitter{-data.jitter.x / static_cast<float>(data.width),
                                               data.jitter.y / static_cast<float>(data.height)};
        const glm::vec2 previousEffectiveJitter{-previousJitter.x / static_cast<float>(data.width),
                                                previousJitter.y / static_cast<float>(data.height)};
        const glm::vec2 effectiveJitterDelta = currentEffectiveJitter - previousEffectiveJitter;
        const gi::RayTracedGiConstants rayTracingConstants{
            .cameraPosition = glm::vec4{data.camera->position(), 1.0f},
            .cameraForward = glm::vec4{data.camera->forward(), 0.0f},
            .toSunWorld = glm::vec4{toSun, 0.0f},
            .sunRadiance = sunRadiance,
            .renderSize = glm::vec4{static_cast<float>(data.width), static_cast<float>(data.height),
                                    effectiveJitterDelta.x, effectiveJitterDelta.y},
            .traceParameters = glm::vec4{0.001f, data.camera->farPlane(),
                                         static_cast<float>(context.identity().sequence.value()), nrdDenoisingRange},
        };
        data.rayTracedSignals = runtime.rayTracedGi->record(
            context.frameGraph(), data.frameIndex, true, rayTracingConstants,
            gi::RayTracedGiFrameGraphInputs{data.hybridSurface.worldPositionHitT, data.hybridSurface.normalRoughness,
                                            data.hybridSurface.albedoMetallic, data.hybridSurface.motion},
            gi::RayTracedGiSceneBindings{
                .descriptors = data.hybridSceneDescriptors,
                .geometry = data.hybridGeometry,
                .baseColorTextures = modelRenderer_->baseColorTextures(),
                .normalRoughnessTextures = modelRenderer_->normalRoughnessTextures(),
                .materialSampler = modelRenderer_->materialSampler(),
            },
            gi::RayTracedGiSceneGraphResources{
                .tlas = data.hybridTlas,
                .instances = data.hybridInstances,
                .materials = data.hybridMaterials,
                .vertices = std::span<const FrameGraphResourceHandle>{data.hybridVertices},
                .indices = std::span<const FrameGraphResourceHandle>{data.hybridIndices},
                .baseColorTextures = std::span<const FrameGraphResourceHandle>{data.hybridBaseColorTextures},
                .normalRoughnessTextures =
                    std::span<const FrameGraphResourceHandle>{data.hybridNormalRoughnessTextures},
                .readyPass = data.hybridSceneReadyPass,
            },
            environment, environmentResources, &*data.sharcRecord);
        static_cast<void>(runtime.sharc->recordStatisticsReadback(context.frameGraph(), *data.sharcRecord,
                                                                  data.rayTracedSignals->tracePass));
        data.hybridGiActive = true;
#endif
    }

    void LevelRenderer::addGiDenoiserFeaturePasses(core::RenderFeatureFrameContext& context) {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        if (!data.hybridGiActive) {
            return;
        }
        HybridGiState& runtime = *hybridGi_;
        if (!runtime.pendingSequence || *runtime.pendingSequence != context.identity().sequence ||
            !runtime.pendingSceneUpdate || !data.rayTracedSignals || runtime.pendingNrdFrame) {
            throw std::logic_error("NRD requires the current frame's RT GI transaction.");
        }

        const core::HistoryAction diffuseAction = context.historyAction(core::HistoryDomain::NrdDiffuse);
        const core::HistoryAction specularAction = context.historyAction(core::HistoryDomain::NrdSpecular);
        const bool resetCameraHistory = !hasSubmittedFrame_ || diffuseAction == core::HistoryAction::FullReset ||
                                        specularAction == core::HistoryAction::FullReset;
        const glm::mat4& previousView = resetCameraHistory ? data.view : previousView_;
        const glm::mat4& previousProjection = resetCameraHistory ? data.projection : previousProjection_;
        const glm::vec2 previousJitter = resetCameraHistory ? data.jitter : previousJitter_;

        gi::NrdCameraData cameraData;
        cameraData.viewToClip = matrixElements(data.projection);
        cameraData.viewToClipPrevious = matrixElements(previousProjection);
        cameraData.worldToView = matrixElements(data.view);
        cameraData.worldToViewPrevious = matrixElements(previousView);
        cameraData.jitter = {data.jitter.x, data.jitter.y};
        cameraData.jitterPrevious = {previousJitter.x, previousJitter.y};

        const gi::RayTracedGiSignalResources& signals = runtime.rayTracedGi->signals(data.frameIndex);
        const gi::RayTracedGiGraphSignals& graphSignals = *data.rayTracedSignals;
        runtime.pendingNrdFrame = runtime.nrd->record(
            context.frameGraph(),
            gi::NrdFrameParameters{
                .frameSlot = context.identity().frameSlot,
                .sequence = context.identity().sequence,
                .extent = context.identity().extent,
                .camera = cameraData,
                .diffuseHistory = diffuseAction,
                .specularHistory = specularAction,
                .cameraCut = context.changes().containsAny(core::HistoryReason::CameraCut),
                .renderResourcesRecreated = context.changes().containsAny(core::HistoryReason::RenderExtentChanged |
                                                                          core::HistoryReason::SwapchainRecreated |
                                                                          core::HistoryReason::DeviceRecovered),
                .frameSlotFenceWaited = true,
                .timeDeltaMilliseconds = 0.0f,
                .denoisingRange = nrdDenoisingRange,
            },
            gi::NrdSignalBindings{
                .diffuseRadianceHitDistance = gi::NrdTextureBinding{signals.diffuseRadianceHitDistance.Get(),
                                                                    graphSignals.diffuseRadianceHitDistance},
                .specularRadianceHitDistance = gi::NrdTextureBinding{signals.specularRadianceHitDistance.Get(),
                                                                     graphSignals.specularRadianceHitDistance},
                .viewZ = gi::NrdTextureBinding{signals.viewZ.Get(), graphSignals.viewZ},
                .normalRoughness = gi::NrdTextureBinding{signals.normalRoughness.Get(), graphSignals.normalRoughness},
                .motion = gi::NrdTextureBinding{signals.motion.Get(), graphSignals.motion},
            });

        const std::span<const FrameGraphPassHandle> nrdPasses = runtime.pendingNrdFrame->passes();
        if (nrdPasses.empty()) {
            throw std::logic_error("NRD did not register any denoising dispatches.");
        }
        const gi::NrdOutputResources& nrdOutputs = runtime.nrd->outputs();
        const gi::NrdGraphOutputs& nrdGraphOutputs = runtime.pendingNrdFrame->outputs();
        const TextureFrameResources& frame = *data.frame;
        const FrameGraphPassHandle compositePass = runtime.composite->record(
            context.frameGraph(),
            gi::GiCompositeFrameParameters{
                .frameSlot = context.identity().frameSlot,
                .extent = context.identity().extent,
                .cameraPosition = data.camera->position(),
                .frameSlotFenceWaited = true,
            },
            gi::GiCompositeResources{
                .diffuseRadianceHitDistance = nrdOutputs.diffuseRadianceHitDistance,
                .specularRadianceHitDistance = nrdOutputs.specularRadianceHitDistance,
                .position = frame.position.texture,
                .normalRoughness = frame.normalRoughness.texture,
                .albedoMetallic = frame.albedo.texture,
                .materialId = frame.materialId.texture,
                .materials = data.hybridSceneDescriptors.materials,
                // Hybrid uses TAA's physical target as an indirect-light scratch texture. The final
                // indirect is staged in TAA's physical target before the Hybrid lighting composite.
                .globalIllumination =
                    data.hybridPathActive ? frame.taaResolved.texture : frame.globalIllumination.texture,
            },
            gi::GiCompositeGraphResources{
                .diffuseRadianceHitDistance = nrdGraphOutputs.diffuseRadianceHitDistance,
                .specularRadianceHitDistance = nrdGraphOutputs.specularRadianceHitDistance,
                .position = data.position,
                .normalRoughness = data.normal,
                .albedoMetallic = data.albedo,
                .materialId = data.materialId,
                .materials = data.hybridMaterials,
                .globalIllumination = data.hybridPathActive ? data.taaResolved : data.globalIllumination,
            },
            nrdPasses.back());
        if (data.hybridPathActive) {
            const gi::RayTracedDiFrameResources& surface = runtime.directLighting->signals(data.frameIndex);
            const FrameGraphPassHandle lightingPass =
                runtime.lightingComposite->record(context.frameGraph(),
                                                  gi::HybridLightingCompositeFrameParameters{
                                                      .frameSlot = context.identity().frameSlot,
                                                      .extent = context.identity().extent,
                                                      .mode = gi::HybridLightingCompositeMode::DirectAndIndirect,
                                                      .frameSlotFenceWaited = true,
                                                  },
                                                  gi::HybridLightingCompositeResources{
                                                      .directRadiance = surface.directRadiance,
                                                      .indirectRadiance = frame.taaResolved.texture,
                                                      .output = frame.lighting.texture,
                                                  },
                                                  gi::HybridLightingCompositeGraphResources{
                                                      .directRadiance = data.hybridSurface.directRadiance,
                                                      .indirectRadiance = data.taaResolved,
                                                      .output = data.lighting,
                                                  },
                                                  compositePass);
            static_cast<void>(lightingPass);
            data.taaInput = data.lighting;
        } else {
            data.taaInput = data.lighting;
        }
#else
        static_cast<void>(context);
#endif
    }

    void LevelRenderer::addSkyCompositeFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        // RT primary miss 已经把 atmosphere environment 写入 directRadiance；Hybrid 的 compute composite
        // 只需等待 GI denoiser，不再声明旧的 raster sky framebuffer/CSM 输入。
        if (data.hybridPathActive) {
            if (data.hybridGiActive) {
                return;
            }
            if (hybridGi_ == nullptr || hybridGi_->lightingComposite == nullptr ||
                hybridGi_->directLighting == nullptr || !data.hybridSurface.directRadiance.isValid() ||
                !data.hybridSurfacePass.isValid()) {
                throw std::logic_error("Hybrid direct-only composite requires the RTDI surface pass.");
            }

            // GI 关闭时仍必须把 RTDI 结果交给 TAA；显式 SSAO 模式则使用 packed GI alpha 作为可见度。
            const bool useAmbientVisibility = data.settings->globalIllumination.enabled &&
                                              data.settings->globalIllumination.mode == GlobalIlluminationMode::Ssao;
            const gi::RayTracedDiFrameResources& surface = hybridGi_->directLighting->signals(data.frameIndex);
            static_cast<void>(hybridGi_->lightingComposite->record(
                context.frameGraph(),
                gi::HybridLightingCompositeFrameParameters{
                    .frameSlot = context.identity().frameSlot,
                    .extent = context.identity().extent,
                    .mode = useAmbientVisibility ? gi::HybridLightingCompositeMode::DirectWithAmbientVisibility
                                                 : gi::HybridLightingCompositeMode::DirectOnly,
                    .frameSlotFenceWaited = true,
                },
                gi::HybridLightingCompositeResources{
                    .directRadiance = surface.directRadiance,
                    .indirectRadiance = data.frame->globalIllumination.texture,
                    .output = data.frame->lighting.texture,
                },
                gi::HybridLightingCompositeGraphResources{
                    .directRadiance = data.hybridSurface.directRadiance,
                    .indirectRadiance = data.globalIllumination,
                    .output = data.lighting,
                },
                data.hybridSurfacePass));
            data.taaInput = data.lighting;
            return;
        }
#endif
        FrameGraph& graph = context.frameGraph();
        if (!data.atmosphereLuts || data.frameIndex >= atmosphereConsumerBindingSets_.size() ||
            !atmosphereConsumerBindingSets_[data.frameIndex]) {
            throw std::logic_error("Sky composite requires the atmosphere LUT record from the current frame.");
        }
        graph.addPass(
            "Procedural sky clear", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.writeTexture(data.lighting, nvrhi::ResourceStates::CopyDest);
            },
            [texture = data.frame->lighting.texture](const FrameGraphContext& frameContext) {
                frameContext.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                            nvrhi::Color{0.035f, 0.04f, 0.05f, 1.0f});
            });
        graph.addPass(
            "Procedural sky", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.globalIllumination, nvrhi::ResourceStates::ShaderResource);
                for (const FrameGraphResourceHandle lut : data.atmosphereLuts->resources.textures) {
                    builder.readTexture(lut, nvrhi::ResourceStates::ShaderResource);
                }
                builder.read(data.atmosphereLuts->resources.constants, nvrhi::ResourceStates::ConstantBuffer);
                builder.writeTexture(data.lighting, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.lightingFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, pipelines_.sky(), data.frameIndex,
                                     atmosphereConsumerBindingSets_[data.frameIndex]);
            });
    }

    void LevelRenderer::addDirectLightingFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (data.hybridPathActive) {
            return;
        }
#endif
        if (modelRenderer_ == nullptr) {
            return;
        }
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "Deferred lighting", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input :
                     {data.position, data.normal, data.albedo, data.globalIllumination, data.materialId}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                builder.read(data.materials, nvrhi::ResourceStates::ShaderResource);
                for (FrameGraphResourceHandle shadow : data.shadows) {
                    builder.readTexture(shadow, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(data.lighting, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.lightingFramebuffer](const FrameGraphContext& frameContext) {
                recordDirectLightingPass(*frameContext.commandList, *framebuffer, data.frameIndex);
            });
    }

    void LevelRenderer::addTemporalAaFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        FrameGraph& graph = context.frameGraph();
        const core::HistoryAction action = context.historyAction(core::HistoryDomain::Taa);
        data.uniforms.renderOptions.x =
            action == core::HistoryAction::Keep && textures_.historyValid(data.historyReadIndex) ? 1.0f : 0.0f;
        textures_.updatePostProcessUniforms(data.frameIndex, data.uniforms);

        graph.addPass(
            "TAA clear", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.writeTexture(data.taaResolved, nvrhi::ResourceStates::CopyDest);
            },
            [texture = data.frame->taaResolved.texture](const FrameGraphContext& frameContext) {
                frameContext.commandList->clearTextureFloat(texture, nvrhi::AllSubresources,
                                                            nvrhi::Color{0.0f, 0.0f, 0.0f, 0.0f});
            });
        graph.addPass(
            "TAA resolve", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                for (FrameGraphResourceHandle input : {data.taaInput, data.motion, data.historyRead}) {
                    builder.readTexture(input, nvrhi::ResourceStates::ShaderResource);
                }
                builder.writeTexture(data.taaResolved, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.taaFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, pipelines_.taa(), data.frameIndex);
            });
        graph.addPass(
            "TAA history copy", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.taaResolved, nvrhi::ResourceStates::CopySource);
                builder.writeTexture(data.historyWrite, nvrhi::ResourceStates::CopyDest);
            },
            [this, &data](const FrameGraphContext& frameContext) {
                recordHistoryCopy(*frameContext.commandList, data.frameIndex);
            });
        graph.addPass(
            "TAA history ready", FrameGraphPassType::Transfer,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.historyWrite, nvrhi::ResourceStates::ShaderResource);
            },
            nullptr);
    }

    void LevelRenderer::addToneMappingFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "Tonemap", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.taaResolved, nvrhi::ResourceStates::ShaderResource);
                builder.readTexture(data.historyWrite, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(data.swap, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data, framebuffer = data.tonemapFramebuffer](const FrameGraphContext& frameContext) {
                recordFullscreenPass(*frameContext.commandList, *framebuffer, pipelines_.tonemap(), data.frameIndex);
            });
    }

    void LevelRenderer::addUiPresentFeaturePasses(core::RenderFeatureFrameContext& context) {
        DeferredFrameData& data = context.blackboard().get<DeferredFrameData>();
        FrameGraph& graph = context.frameGraph();
        graph.addPass(
            "ImGui overlay", FrameGraphPassType::Graphics,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.imguiFont, nvrhi::ResourceStates::ShaderResource);
                builder.writeTexture(data.swap, nvrhi::ResourceStates::RenderTarget);
            },
            [this, &data](const FrameGraphContext& frameContext) {
                imgui_.record(*frameContext.commandList, data.imageIndex, data.frameIndex);
            });
        graph.addPass(
            "Present", FrameGraphPassType::Present,
            [&data](FrameGraphBuilder& builder) {
                builder.readTexture(data.swap, nvrhi::ResourceStates::Present);
            },
            nullptr);
    }

    void LevelRenderer::recordShadowPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                         std::uint32_t frameIndex, std::uint32_t cascadeIndex,
                                         const glm::mat4& lightViewProjection) {
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordShadow(commandList, framebuffer, shadowMapResolution, shadowMapResolution, frameIndex,
                                         cascadeIndex, lightViewProjection);
        }
    }

    void LevelRenderer::recordGBufferPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                          std::uint32_t frameIndex, const glm::mat4& viewProjection,
                                          const glm::mat4& previousViewProjection) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        if (modelRenderer_ != nullptr) {
            modelRenderer_->recordGBuffer(commandList, framebuffer, frame.position.width, frame.position.height,
                                          frameIndex, viewProjection, previousViewProjection);
        }
    }

    void LevelRenderer::recordFullscreenPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                             const nvrhi::GraphicsPipelineHandle& pipeline, std::uint32_t frameIndex,
                                             const nvrhi::BindingSetHandle& additionalBindingSet) {
        const std::uint32_t width = context_.swapchainWidth();
        const std::uint32_t height = context_.swapchainHeight();
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
            .setFramebuffer(&framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))))
            .addBindingSet(textures_.bindingSet(frameIndex));
        if (additionalBindingSet) {
            state.addBindingSet(additionalBindingSet);
        }
        commandList.setGraphicsState(state);
        commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
    }

    void LevelRenderer::recordDirectLightingPass(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                                                 std::uint32_t frameIndex) {
        if (frameIndex >= directLightingBindingSets_.size() || !directLightingBindingSets_[frameIndex]) {
            throw std::logic_error("Direct-lighting material bindings are unavailable for the frame slot.");
        }
        nvrhi::GraphicsState state;
        state.setPipeline(pipelines_.deferredLighting())
            .setFramebuffer(&framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(
                static_cast<float>(context_.swapchainWidth()), static_cast<float>(context_.swapchainHeight()))))
            .addBindingSet(textures_.bindingSet(frameIndex))
            .addBindingSet(directLightingBindingSets_[frameIndex]);
        commandList.setGraphicsState(state);
        commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
    }

    void LevelRenderer::recordHistoryCopy(nvrhi::ICommandList& commandList, std::uint32_t frameIndex) {
        const TextureFrameResources& frame = textures_.frame(frameIndex);
        commandList.copyTexture(frame.history.texture, nvrhi::TextureSlice{}, frame.taaResolved.texture,
                                nvrhi::TextureSlice{});
    }

} // namespace lumin::render

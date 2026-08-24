#include "render/level/FeatureFrameData.hpp"
#include "render/pipelines/default/DefaultRenderPipelineSession.hpp"

#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/Level.hpp"

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
#include "render/gi/raytracing/HybridLightingComposite.hpp"
#include "render/gi/raytracing/RayTracedDirectLighting.hpp"
#include "render/gi/raytracing/SharcIndirectLighting.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lumin::render {

    void pipelines::DefaultRenderPipelineSession::createRenderResources() {
        const std::uint32_t width = renderExtent_.width;
        const std::uint32_t height = renderExtent_.height;
        rasterResources_.create(width, height);
        std::array<PostFxBindingInputs, frameSlotCount> postFxInputs{};
        for (std::uint32_t frameIndex = 0; frameIndex < frameSlotCount; ++frameIndex) {
            const RasterFeatureFrameResources& raster = rasterResources_.frame(frameIndex);
            postFxInputs[frameIndex].surfaces = {raster.position.texture, raster.normalRoughness.texture,
                                                 raster.albedo.texture, raster.motion.texture};
            for (std::uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade) {
                postFxInputs[frameIndex].shadows[cascade] = raster.shadowCascades[cascade].texture;
            }
        }
        postFxResources_.create(width, height, postFxInputs);
        createViewportOutput();
        createDirectLightingBindingLayout();
        atmosphereLutGpu_ = std::make_unique<atmosphere::AtmosphereLutGpu>(atmosphere::AtmosphereLutGpuCreateInfo{
            .device = context_.rhiDevice(),
            .shaders = &shaderLibrary_,
            .frameSlotCount = frameSlotCount,
            .quality = {},
        });
        createAtmosphereConsumerBindings();
        const std::array<nvrhi::BindingLayoutHandle, 1> fullscreenLayouts = {postFxResources_.bindingLayout()};
        const std::array<nvrhi::BindingLayoutHandle, 2> lightingLayouts = {postFxResources_.bindingLayout(),
                                                                           directLightingBindingLayout_};
        const std::array<nvrhi::BindingLayoutHandle, 2> skyLayouts = {postFxResources_.bindingLayout(),
                                                                      atmosphereConsumerBindingLayout_};
        skyPipeline_ = fullscreenPipelineFactory_.create(ShaderId::SkyVertex, ShaderId::SkyFragment,
                                                         postFxResources_.lightingFormat(), skyLayouts);
        directLightingPipeline_ = fullscreenPipelineFactory_.create(
            ShaderId::DeferredVertex, ShaderId::DeferredFragment, postFxResources_.lightingFormat(), lightingLayouts);
        temporalAaPipeline_ = fullscreenPipelineFactory_.create(ShaderId::TaaVertex, ShaderId::TaaFragment,
                                                                postFxResources_.lightingFormat(), fullscreenLayouts);
        toneMappingPipeline_ =
            fullscreenPipelineFactory_.create(ShaderId::PostProcessVertex, ShaderId::PostProcessFragment,
                                              context_.swapchainRhiFormat(), fullscreenLayouts);

        std::array<gi::FrameResources, frameSlotCount> giFrames{};
        for (std::uint32_t frameIndex = 0; frameIndex < giFrames.size(); ++frameIndex) {
            const RasterFeatureFrameResources& raster = rasterResources_.frame(frameIndex);
            const PostFxFrameResources& postFx = postFxResources_.frame(frameIndex);
            giFrames[frameIndex].position = raster.position.texture;
            giFrames[frameIndex].normalRoughness = raster.normalRoughness.texture;
            giFrames[frameIndex].albedoMetallic = raster.albedo.texture;
            giFrames[frameIndex].motion = raster.motion.texture;
            giFrames[frameIndex].depth = raster.depth.texture;
            giFrames[frameIndex].uniformBuffer = postFx.uniforms.buffer;
            giFrames[frameIndex].output = postFx.globalIllumination.texture;
        }
        globalIllumination_->create(gi::CreateInfo{.device = context_.rhiDevice(),
                                                   .shaders = &shaderLibrary_,
                                                   .extent = {width, height},
                                                   .outputFormat = postFxResources_.globalIlluminationFormat(),
                                                   .sampler = postFxResources_.sampler(),
                                                   .frames = giFrames});
        atmosphereForceRebuild_ = true;
        createModelRenderer();
        createHybridGiResources();
        frameResourcesInitialized_.fill(false);
    }

    void pipelines::DefaultRenderPipelineSession::createViewportOutput() {
        nvrhi::TextureDesc desc;
        desc.setWidth(renderExtent_.width)
            .setHeight(renderExtent_.height)
            .setFormat(context_.swapchainRhiFormat())
            .setDebugName("Editor Viewport output")
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::Common)
            .setKeepInitialState(false);
        viewportOutput_.texture = context_.rhiDevice()->createTexture(desc);
        if (!viewportOutput_.texture) {
            throw std::runtime_error("Failed to create the editor Viewport output texture.");
        }
        viewportOutput_.format = desc.format;
        viewportOutput_.width = desc.width;
        viewportOutput_.height = desc.height;
        viewportOutput_.initialState = desc.initialState;
        viewportOutputInitialized_ = false;
    }

    void pipelines::DefaultRenderPipelineSession::createModelRenderer() {
        const world::RenderWorldSnapshotPtr snapshot = currentWorld_;
        if (snapshot == nullptr) {
            throw std::logic_error("Default pipeline session requires a synchronized render-world snapshot.");
        }
        if (snapshot->instances().empty()) {
            modelRenderer_.reset();
            return;
        }
        const std::array<nvrhi::Format, 5> colorFormats = {
            rasterResources_.positionFormat(), rasterResources_.normalFormat(), rasterResources_.albedoFormat(),
            rasterResources_.motionFormat(), rasterResources_.materialIdFormat()};
        modelRenderer_ = std::make_unique<ModelRenderer>(
            context_, *snapshot, shaderLibrary_, colorFormats, rasterResources_.depthFormat(),
            rasterResources_.shadowDepthFormat(), frameSlotCount, context_.modelRendererCapabilities());
        createDirectLightingBindingSets();
    }

    void pipelines::DefaultRenderPipelineSession::createDirectLightingBindingLayout() {
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

    void pipelines::DefaultRenderPipelineSession::createDirectLightingBindingSets() {
        directLightingBindingSets_.fill(nullptr);
        if (modelRenderer_ == nullptr) {
            return;
        }
        for (std::uint32_t frameIndex = 0; frameIndex < directLightingBindingSets_.size(); ++frameIndex) {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, rasterResources_.frame(frameIndex).materialId.texture))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, modelRenderer_->materialBuffer(frameIndex)));
            directLightingBindingSets_[frameIndex] =
                context_.rhiDevice()->createBindingSet(desc, directLightingBindingLayout_);
            if (!directLightingBindingSets_[frameIndex]) {
                directLightingBindingSets_.fill(nullptr);
                throw std::runtime_error("Failed to create a direct-lighting material binding set.");
            }
        }
    }

    void pipelines::DefaultRenderPipelineSession::createAtmosphereConsumerBindings() {
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

    void pipelines::DefaultRenderPipelineSession::createHybridGiResources() {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        const auto buildCandidate = [this]() -> std::unique_ptr<HybridGiState> {
            if (!context_.rayTracingDecision().enabled() ||
                !context_.rayTracingSupport().supportsSharcShaderStorage()) {
                return {};
            }
            const world::RenderWorldSnapshotPtr snapshot = currentWorld_;
            if (snapshot == nullptr) {
                throw std::logic_error("Hybrid GI requires a synchronized render-world snapshot.");
            }
            if (modelRenderer_ == nullptr || modelRenderer_->baseColorTextures().empty() ||
                modelRenderer_->baseColorTextures().size() > std::numeric_limits<std::uint16_t>::max()) {
                return {};
            }
            const std::size_t requestedCapacity = std::max<std::size_t>(snapshot->meshes().size(), 1U);
            if (requestedCapacity > std::numeric_limits<std::uint16_t>::max()) {
                // NvRHI descriptor-array size is uint16_t；超大场景保留 SSAO 回退，不创建部分可用的 RT runtime。
                return {};
            }

            auto runtime = std::make_unique<HybridGiState>();
            runtime->sharcEnabled = requestedSharcEnabled_;
            runtime->geometryDescriptorCapacity = static_cast<std::uint32_t>(requestedCapacity);
            const std::uint32_t materialTextureDescriptorCapacity =
                static_cast<std::uint32_t>(modelRenderer_->baseColorTextures().size());
            runtime->sceneBackend = std::make_unique<gpu::NvrhiGpuSceneBackend>(*context_.rhiDevice());
            runtime->sceneResources = std::make_unique<gpu::GpuSceneResources>(
                *runtime->sceneBackend, gpu::GpuSceneResourceConfig{frameSlotCount, true});
            runtime->scenePlanner = std::make_unique<gpu::GpuSceneUpdatePlanner>();

            for (std::uint32_t frameIndex = 0; frameIndex < frameSlotCount; ++frameIndex) {
                const RasterFeatureFrameResources& raster = rasterResources_.frame(frameIndex);
                const PostFxFrameResources& postFx = postFxResources_.frame(frameIndex);
                runtime->directLightingFrames[frameIndex] = gi::RayTracedDiFrameResources{
                    .worldPositionHitT = raster.position.texture,
                    .normalRoughness = raster.normalRoughness.texture,
                    .albedoMetallic = raster.albedo.texture,
                    .materialId = raster.materialId.texture,
                    .viewZ = {},
                    .motion = raster.motion.texture,
                    // Hybrid RTDI 使用 globalIllumination 作为 direct-radiance UAV，最终合成再写 lighting。
                    .directRadiance = postFx.globalIllumination.texture,
                    .visibilityMask = {},
                };
            }
            runtime->directLighting =
                std::make_unique<gi::RayTracedDirectLightingPass>(gi::RayTracedDirectLightingPass::CreateInfo{
                    .device = context_.rhiDevice(),
                    .shaders = &shaderLibrary_,
                    .width = renderExtent_.width,
                    .height = renderExtent_.height,
                    .maxGeometryDescriptors = runtime->geometryDescriptorCapacity,
                    .maxMaterialTextureDescriptors = materialTextureDescriptorCapacity,
                    .atmosphereBindingLayout = atmosphereConsumerBindingLayout_,
                    .frames = runtime->directLightingFrames,
                });

            std::array<gi::SharcIndirectLightingFrameInputs, frameSlotCount> indirectFrames{};
            std::array<gi::SharcUpdateFrameInputs, frameSlotCount> sharcFrames{};
            for (std::uint32_t frameIndex = 0; frameIndex < frameSlotCount; ++frameIndex) {
                const RasterFeatureFrameResources& frame = rasterResources_.frame(frameIndex);
                indirectFrames[frameIndex] = gi::SharcIndirectLightingFrameInputs{
                    .position = frame.position.texture,
                    .normalRoughness = frame.normalRoughness.texture,
                    .albedoMetallic = frame.albedo.texture,
                    .motion = frame.motion.texture,
                    .materialId = frame.materialId.texture,
                };
                sharcFrames[frameIndex] = gi::SharcUpdateFrameInputs{
                    .position = frame.position.texture,
                    .normalRoughness = frame.normalRoughness.texture,
                    .albedoMetallic = frame.albedo.texture,
                    .materialId = frame.materialId.texture,
                };
            }

            runtime->sharc = std::make_unique<gi::SharcRadianceCache>(gi::SharcRadianceCacheCreateInfo{
                .device = context_.rhiDevice(),
                .shaders = &shaderLibrary_,
                .frameSlotCount = frameSlotCount,
                .maxGeometryDescriptors = runtime->geometryDescriptorCapacity,
                .maxMaterialTextureDescriptors = materialTextureDescriptorCapacity,
                .atmosphereBindingLayout = atmosphereConsumerBindingLayout_,
                .config = {},
                .frames = sharcFrames,
            });
            runtime->indirectLighting =
                std::make_unique<gi::SharcIndirectLightingPass>(gi::SharcIndirectLightingCreateInfo{
                    .device = context_.rhiDevice(),
                    .shaders = &shaderLibrary_,
                    .width = renderExtent_.width,
                    .height = renderExtent_.height,
                    .maxGeometryDescriptors = runtime->geometryDescriptorCapacity,
                    .maxMaterialTextureDescriptors = materialTextureDescriptorCapacity,
                    .atmosphereBindingLayout = atmosphereConsumerBindingLayout_,
                    .frames = indirectFrames,
                });
            runtime->nrd = std::make_unique<gi::NrdDenoiser>(gi::NrdDenoiserCreateInfo{
                .device = context_.rhiDevice(),
                .extent = renderExtent_,
                .frameSlotCount = frameSlotCount,
            });
            if (runtime->indirectLighting->formats().normalRoughness != runtime->nrd->expectedNormalRoughnessFormat()) {
                throw std::runtime_error("SHARC indirect and NRD normal/roughness formats do not match.");
            }
            runtime->composite = std::make_unique<gi::GiCompositePass>(gi::GiCompositeCreateInfo{
                .device = context_.rhiDevice(),
                .shaders = &shaderLibrary_,
                .extent = renderExtent_,
                .frameSlotCount = frameSlotCount,
            });
            runtime->lightingComposite =
                std::make_unique<gi::HybridLightingCompositePass>(gi::HybridLightingCompositeCreateInfo{
                    .device = context_.rhiDevice(),
                    .shaders = &shaderLibrary_,
                    .extent = renderExtent_,
                    .frameSlotCount = frameSlotCount,
                });
            return runtime;
        };

        // 候选完整创建前保留旧资源；异常会自然销毁候选并让旧状态继续服务后续帧。
        std::unique_ptr<HybridGiState> candidate = buildCandidate();
        hybridGi_ = std::move(candidate);
        lastSubmittedFrameUsedHybridGi_ = false;
#else
        destroyHybridGiResources();
#endif
    }

    void pipelines::DefaultRenderPipelineSession::ensureHybridGiCapacity() {
#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
        if (!context_.rayTracingDecision().enabled() || !context_.rayTracingSupport().supportsSharcShaderStorage()) {
            destroyHybridGiResources();
            return;
        }
        const world::RenderWorldSnapshotPtr snapshot = currentWorld_;
        if (snapshot == nullptr) {
            throw std::logic_error("Hybrid GI capacity check requires a render-world snapshot.");
        }
        const std::size_t requiredCapacity = std::max<std::size_t>(snapshot->meshes().size(), 1U);
        if (requiredCapacity > std::numeric_limits<std::uint16_t>::max()) {
            destroyHybridGiResources();
            return;
        }
        // ModelRenderer 拓扑已在调用前重建，即使容量未增长也必须重绑新材质纹理和几何资源。
        createHybridGiResources();
#endif
    }

    void pipelines::DefaultRenderPipelineSession::destroyDirectLightingBindings() noexcept {
        directLightingBindingSets_.fill(nullptr);
        directLightingBindingLayout_ = nullptr;
    }

    void pipelines::DefaultRenderPipelineSession::destroyHybridGiResources() noexcept {
        hybridGi_.reset();
        lastSubmittedFrameUsedHybridGi_ = false;
    }

    void pipelines::DefaultRenderPipelineSession::destroyRenderResources() noexcept {
        frameGraph_.reset();
        destroyHybridGiResources();
        directLightingBindingSets_.fill(nullptr);
        modelRenderer_.reset();
        toneMappingPipeline_ = nullptr;
        temporalAaPipeline_ = nullptr;
        directLightingPipeline_ = nullptr;
        skyPipeline_ = nullptr;
        atmosphereConsumerBindingSets_.fill(nullptr);
        atmosphereConsumerBindingLayout_ = nullptr;
        atmosphereLutGpu_.reset();
        atmosphereLutScheduler_ = {};
        atmosphereForceRebuild_ = true;
        globalIllumination_->destroy();
        destroyDirectLightingBindings();
        // PostFX binding sets 强引用 Raster 纹理，销毁顺序必须保持消费者先于 producer。
        postFxResources_.destroy();
        rasterResources_.destroy();
        viewportOutput_ = {};
        viewportOutputInitialized_ = false;
    }

    void pipelines::DefaultRenderPipelineSession::applyPendingViewportExtent() {
        if (requestedExtentStableFrames_ < 2 || requestedRenderExtent_ == renderExtent_) {
            return;
        }
        renderPipeline_->discardFrame();
        context_.waitIdle();
        presentation_.setViewportTexture(nullptr);
        destroyRenderResources();
        renderExtent_ = requestedRenderExtent_;
        pendingFrameChanges_.add(core::HistoryReason::RenderExtentChanged);
        createRenderResources();
        presentation_.setViewportTexture(viewportOutput_.texture);
    }

    void pipelines::DefaultRenderPipelineSession::refreshSwapchainResources() {
        renderPipeline_->discardFrame();
        pendingFrameChanges_.add(core::HistoryReason::SwapchainRecreated);
        context_.waitIdle();
        const bool viewportFormatChanged = viewportOutput_.format != context_.swapchainRhiFormat();
        presentation_.shutdown();
        if (viewportFormatChanged) {
            destroyRenderResources();
            createRenderResources();
        }
        presentation_.initialize(context_, *uiFontAtlas_, shaderLibrary_);
        presentation_.setViewportTexture(viewportOutput_.texture);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

} // namespace lumin::render

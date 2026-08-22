#include "render/level/FeatureFrameData.hpp"
#include "render/level/LevelRendererImpl.hpp"

#include "render/platform/vulkan/VulkanContext.hpp"
#include "scene/Level.hpp"

#if LUMIN_LEVEL_RENDERER_HAS_HYBRID_GI
#include "render/gi/raytracing/HybridLightingComposite.hpp"
#include "render/gi/raytracing/RayTracedDirectLighting.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lumin::render {

    void LevelRenderer::Impl::createRenderResources() {
        const std::uint32_t width = renderExtent_.width;
        const std::uint32_t height = renderExtent_.height;
        textures_.create(width, height);
        createViewportOutput();
        createDirectLightingBindingLayout();
        atmosphereLutGpu_ = std::make_unique<atmosphere::AtmosphereLutGpu>(atmosphere::AtmosphereLutGpuCreateInfo{
            .device = context_.rhiDevice(),
            .shaderDirectory = shaderDirectory_,
            .frameSlotCount = TextureManager::maxFramesInFlight,
            .quality = {},
        });
        createAtmosphereConsumerBindings();
        const std::array<nvrhi::BindingLayoutHandle, 1> fullscreenLayouts = {textures_.bindingLayout()};
        const std::array<nvrhi::BindingLayoutHandle, 2> lightingLayouts = {textures_.bindingLayout(),
                                                                           directLightingBindingLayout_};
        const std::array<nvrhi::BindingLayoutHandle, 2> skyLayouts = {textures_.bindingLayout(),
                                                                      atmosphereConsumerBindingLayout_};
        skyPipeline_ = fullscreenPipelineFactory_.create("Sky", textures_.lightingFormat(), skyLayouts);
        directLightingPipeline_ =
            fullscreenPipelineFactory_.create("Deferred", textures_.lightingFormat(), lightingLayouts);
        temporalAaPipeline_ = fullscreenPipelineFactory_.create("Taa", textures_.lightingFormat(), fullscreenLayouts);
        toneMappingPipeline_ =
            fullscreenPipelineFactory_.create("PostProcess", context_.swapchainRhiFormat(), fullscreenLayouts);

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

    void LevelRenderer::Impl::createViewportOutput() {
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

    void LevelRenderer::Impl::createModelRenderer() {
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

    void LevelRenderer::Impl::createDirectLightingBindingLayout() {
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

    void LevelRenderer::Impl::createDirectLightingBindingSets() {
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

    void LevelRenderer::Impl::createAtmosphereConsumerBindings() {
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

    void LevelRenderer::Impl::createHybridGiResources() {
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
        runtime->sharcEnabled = requestedSharcEnabled_;
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
                .width = renderExtent_.width,
                .height = renderExtent_.height,
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
            .width = renderExtent_.width,
            .height = renderExtent_.height,
            .maxGeometryDescriptors = runtime->geometryDescriptorCapacity,
            .maxMaterialTextureDescriptors = materialTextureDescriptorCapacity,
            .atmosphereBindingLayout = atmosphereConsumerBindingLayout_,
            .enableSharc = runtime->sharcEnabled,
            .frames = rayTracedFrames,
        });
        runtime->nrd = std::make_unique<gi::NrdDenoiser>(gi::NrdDenoiserCreateInfo{
            .device = context_.rhiDevice(),
            .extent = renderExtent_,
            .frameSlotCount = TextureManager::maxFramesInFlight,
        });
        if (runtime->rayTracedGi->formats().normalRoughness != runtime->nrd->expectedNormalRoughnessFormat()) {
            throw std::runtime_error("RT GI and NRD normal/roughness formats do not match.");
        }
        runtime->composite = std::make_unique<gi::GiCompositePass>(gi::GiCompositeCreateInfo{
            .device = context_.rhiDevice(),
            .shaderDirectory = shaderDirectory_,
            .extent = renderExtent_,
            .frameSlotCount = TextureManager::maxFramesInFlight,
        });
        runtime->lightingComposite =
            std::make_unique<gi::HybridLightingCompositePass>(gi::HybridLightingCompositeCreateInfo{
                .device = context_.rhiDevice(),
                .shaderDirectory = shaderDirectory_,
                .extent = renderExtent_,
                .frameSlotCount = TextureManager::maxFramesInFlight,
            });
        hybridGi_ = std::move(runtime);
#endif
    }

    void LevelRenderer::Impl::ensureHybridGiCapacity() {
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

    void LevelRenderer::Impl::destroyDirectLightingBindings() noexcept {
        directLightingBindingSets_.fill(nullptr);
        directLightingBindingLayout_ = nullptr;
    }

    void LevelRenderer::Impl::destroyHybridGiResources() noexcept {
        hybridGi_.reset();
        lastSubmittedFrameUsedHybridGi_ = false;
    }

    void LevelRenderer::Impl::destroyRenderResources() noexcept {
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
        textures_.destroy();
        viewportOutput_ = {};
        viewportOutputInitialized_ = false;
    }

    void LevelRenderer::Impl::applyPendingViewportExtent() {
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

    void LevelRenderer::Impl::refreshSwapchainResources() {
        renderPipeline_->discardFrame();
        pendingFrameChanges_.add(core::HistoryReason::SwapchainRecreated);
        context_.waitIdle();
        const bool viewportFormatChanged = viewportOutput_.format != context_.swapchainRhiFormat();
        presentation_.shutdown();
        if (viewportFormatChanged) {
            destroyRenderResources();
            createRenderResources();
        }
        presentation_.initialize(context_, uiFontAtlas_, shaderDirectory_);
        presentation_.setViewportTexture(viewportOutput_.texture);
        swapchainGeneration_ = context_.swapchainGeneration();
    }

} // namespace lumin::render

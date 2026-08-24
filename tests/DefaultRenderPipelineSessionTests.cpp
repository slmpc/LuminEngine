#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef LUMIN_TEST_SOURCE_DIR
#define LUMIN_TEST_SOURCE_DIR "."
#endif

namespace {

    std::string readSource(const std::string& relativePath) {
        const std::string path = std::string{LUMIN_TEST_SOURCE_DIR} + "/" + relativePath;
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Unable to open production source: " + path);
        }
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(std::string{message});
        }
    }

    std::size_t requireAfter(const std::string& source, const std::string& token, std::size_t position) {
        const std::size_t found = source.find(token, position);
        require(found != std::string::npos, std::string{"Missing production recorder token: "} + std::string{token});
        return found + token.size();
    }

    std::size_t countOccurrences(const std::string& source, const std::string& token) {
        std::size_t count = 0;
        for (std::size_t position = 0; (position = source.find(token, position)) != std::string::npos;
             position += token.size()) {
            ++count;
        }
        return count;
    }

    void verifyPassOrder(const std::string& level, const std::string& pipelineDefinition) {
        std::size_t position = level.find("pipelines::DefaultRenderPipelineSession::recordCommandList");
        require(position != std::string::npos, "NvRHI frame recorder entry point is missing.");
        for (const std::string& pass : std::vector<std::string>{
                 "CSM cascade ", "G-buffer", "globalIllumination_->addPasses", "Procedural sky", "Deferred lighting",
                 "TAA resolve", "TAA history copy", "TAA history ready", "Bloom downsample ", "Bloom upsample ",
                 "Bloom composite", "Tonemap", "ImGui overlay", "Present"}) {
            position = requireAfter(level, pass, position);
        }
        require(level.find("builder.readTexture(lighting.combined.graphResource") != std::string::npos,
                "Sky must read the GI output to preserve its FrameGraph dependency.");
        require(level.find("builder.readTexture(bloom.color.graphResource") != std::string::npos,
                "Tonemap must read the completed Bloom HDR output.");
        require(
            level.find("builder.readTexture(viewport.color.graphResource, nvrhi::ResourceStates::ShaderResource)") !=
                std::string::npos,
            "ImGui must sample the completed Viewport output before presentation.");

        for (const std::string& contract :
             {"frame_data::atmosphere()", "frame_data::shadows()", "frame_data::rasterSurface()",
              "frame_data::indirectLighting()", "frame_data::denoisedLighting()", "frame_data::sceneHdr()",
              "frame_data::temporalOutput()", "frame_data::bloomOutput()", "frame_data::viewportOutput()",
              "frame_data::present()"}) {
            require(pipelineDefinition.find(contract) != std::string::npos,
                    "Default recipes must declare every typed producer output.");
        }
        std::cout << "PASS_ORDER=CSMx4>G-buffer>GI>sky>deferred>TAA>Bloom6x5>tonemap>ImGui>Present\n";
    }

    void verifyNvrhiRecording(const std::string& level) {
        require(level.find("frame->commandList") != std::string::npos,
                "drawFrame must use the active NvRHI command list.");
        require(level.find("recordHistoryCopy") != std::string::npos &&
                    level.find("frame.history.texture, nvrhi::TextureSlice{}, frame.taaResolved.texture") !=
                        std::string::npos,
                "TAA history must be copied explicitly from the unsharpened resolve.");
        require(countOccurrences(level, "copyTexture") == 2,
                "Only TAA history and the disabled-Bloom bypass may use texture copies.");
        require(level.find("ResourceStates::CopySource") != std::string::npos,
                "TAA history source must be declared CopySource.");
        require(level.find("ResourceStates::CopyDest") != std::string::npos,
                "TAA history destination must be declared CopyDest.");
        require(level.find("context_.modelRendererCapabilities()") != std::string::npos,
                "Default pipeline must hand explicit device-derived ModelRendererCapabilities to ModelRenderer.");
        require(level.find("textureDesc(rasterFrame.materialId") != std::string::npos &&
                    level.find("addColorAttachment(rasterFrame.materialId.texture)") != std::string::npos &&
                    level.find("clearTextureUInt(surface.materialId.texture") != std::string::npos &&
                    level.find("GpuMaterialIndex::invalidValue") != std::string::npos,
                "The G-buffer must own, attach, and integer-clear a dedicated stable material-ID texture.");
        require(
            level.find("modelRenderer_->materialBufferInitialState(frameIndex)") != std::string::npos &&
                level.find("builder.read(surface.materials.graphResource, nvrhi::ResourceStates::ShaderResource)") !=
                    std::string::npos,
            "Direct lighting must import and declare the frame-slot material buffer through FrameGraph.");
        require(level.find("setRegisterSpaceAndDescriptorSet(1)") != std::string::npos &&
                    level.find("addBindingSet(directLightingBindingSets_[frameIndex])") != std::string::npos,
                "Deferred direct lighting must bind its material resources in descriptor set 1.");
        require(level.find("? nvrhi::ResourceStates::DepthWrite") != std::string::npos &&
                    level.find("textureDesc(rasterFrame.depth, depthInitialState)") != std::string::npos,
                "The non-sampled G-buffer depth texture must retain DepthWrite across frame-slot reuse.");
        for (const std::string& pass : {"CSM clear", "G-buffer clear", "Procedural sky clear", "TAA clear"}) {
            const std::size_t name = level.find(pass);
            require(name != std::string::npos, "Every runtime attachment clear needs a dedicated FrameGraph pass.");
            const std::size_t copyDest = level.find("ResourceStates::CopyDest", name);
            const std::size_t addPassEnd = level.find("graph.addPass", name + 1);
            require(copyDest != std::string::npos && (addPassEnd == std::string::npos || copyDest < addPassEnd),
                    "Runtime clear passes must declare their textures as CopyDest.");
        }
        require(
            level.find("UI background clear") == std::string::npos &&
                level.find("builder.writeTexture(input.swapchain.graphResource, nvrhi::ResourceStates::CopyDest)") ==
                    std::string::npos,
            "Swapchain images only guarantee color-attachment usage and must not be transfer-cleared.");
        const std::size_t tonemap = level.find("\"Tonemap\", FrameGraphPassType::Graphics");
        const std::size_t tonemapRenderTarget = level.find(
            "builder.writeTexture(viewport.color.graphResource, nvrhi::ResourceStates::RenderTarget)", tonemap);
        require(tonemap != std::string::npos && tonemapRenderTarget != std::string::npos,
                "Tonemap must write the renderer-owned Viewport output.");
        const std::size_t imgui = level.find("\"ImGui overlay\", FrameGraphPassType::Graphics", tonemap);
        const std::size_t imguiSwap = level.find(
            "builder.writeTexture(input.swapchain.graphResource, nvrhi::ResourceStates::RenderTarget)", imgui);
        require(imgui != std::string::npos && imguiSwap != std::string::npos,
                "Only the ImGui composition pass may write the swapchain render target.");
        for (const std::string& forbidden :
             std::vector<std::string>{"vkCmd", "VkRendering", "VkDescriptorSet", "VkPipeline", "VkCommandBuffer"}) {
            require(level.find(forbidden) == std::string::npos,
                    std::string{"Forbidden direct Vulkan recorder token remains: "} + std::string{forbidden});
        }
        std::cout << "COPY_PASSES=TAA-history,Bloom-bypass\nMODEL_CAPABILITIES=explicit-device-derived\n";
    }

    void verifyHistoryAndErrorPaths(const std::string& level) {
        for (const std::string& token : std::vector<std::string>{
                 "pendingFrameChanges_.merge(core::frameChangesFromScene(sceneChanges))",
                 "HistoryReason::FeatureConfigurationChanged", "HistoryReason::SwapchainRecreated",
                 "core::RenderFrameIdentity identity", "core::FrameSlotIndex{frame->frameIndex}",
                 "core::SwapImageIndex{frame->imageIndex}", "core::RenderSequence{nextRenderSequence_}",
                 "renderPipeline_->prepareFrame", "renderPipeline_->discardFrame"}) {
            require(level.find(token) != std::string::npos, "Required transactional history token is missing.");
        }
        const std::size_t submit = level.find("context_.submitFrameCommands");
        const std::size_t pipelineCommit = level.find("renderPipeline_->commitFrame(identity)", submit);
        const std::size_t changesConsumed = level.find("pendingFrameChanges_.clear()", pipelineCommit);
        const std::size_t sequenceAdvance = level.find("++nextRenderSequence_", changesConsumed);
        const std::size_t present = level.find("context_.presentFrame", sequenceAdvance);
        require(
            submit != std::string::npos && pipelineCommit != std::string::npos &&
                changesConsumed != std::string::npos && sequenceAdvance != std::string::npos &&
                present != std::string::npos && submit < pipelineCommit && pipelineCommit < changesConsumed &&
                changesConsumed < sequenceAdvance && sequenceAdvance < present,
            "Feature history must commit after queue submit and before present, whose failure cannot roll it back.");
        const std::size_t rasterFeature = level.find("class DefaultRenderPipelineSession::RasterSurfaceFeatureModule");
        const std::size_t rasterCommit = level.find("modelRenderer_->commitSubmittedFrame", rasterFeature);
        const std::size_t rasterDiscard = level.find("modelRenderer_->discardPendingFrame", rasterCommit);
        const std::size_t temporalFeature = level.find("class DefaultRenderPipelineSession::TemporalAaFeatureModule");
        const std::size_t temporalCommit = level.find("postFxResources_.markHistoryValid", temporalFeature);
        require(rasterFeature != std::string::npos && rasterCommit != std::string::npos &&
                    rasterDiscard != std::string::npos && temporalFeature != std::string::npos &&
                    temporalCommit != std::string::npos && rasterFeature < rasterCommit &&
                    rasterCommit < rasterDiscard && temporalFeature < temporalCommit,
                "Model and TAA histories must be committed through independent Feature submission notifications.");
        require(level.find("LevelRenderFeatureKind") == std::string::npos &&
                    level.find("switch (kind)") == std::string::npos,
                "Runtime must not route Feature lifecycle through a central enum switch.");
        require(level.find("modelRenderer_->discardPendingFrame") != std::string::npos,
                "Discarded Feature frames must abandon pending model transforms.");
        require(level.find("postFxResources_.invalidateHistory") == std::string::npos &&
                    level.find("gi::shouldInvalidateHistory") == std::string::npos,
                "Default pipeline must not retain ad hoc TAA or GI invalidation decisions.");
        require(countOccurrences(level, "renderPipeline_->discardFrame") >= 3,
                "Record, submit and swapchain-rebuild paths must all discard pending Feature work.");
        require(level.find("viewportOutput_.format != context_.swapchainRhiFormat()") != std::string::npos,
                "Swapchain resize must preserve Viewport render resources unless the surface format changes.");
        require(level.find("context_.cancelFrame") != std::string::npos,
                "Record failure must cancel the acquired frame through VulkanContext.");
        require(
            level.find("ImGui::") == std::string::npos && level.find("ImGuiManager") == std::string::npos,
            "Default pipeline orchestration must pass current-frame ImDrawData without reading global ImGui state.");
        std::cout << "HISTORY_COORDINATION=FrameChangeSet>prepare>submit>commit;SEQUENCE=success-only\n"
                     "ERROR_PATH=acquire/record/submit-discard,retry\n";
    }

    void verifyRenderWorldSnapshotBoundary(const std::string& level) {
        require(level.find("world::changesBetween(committedWorld_, packet.world)") != std::string::npos &&
                    level.find("committedWorld_ = packet.world") != std::string::npos,
                "Runtime must compare packet snapshots with the last successfully submitted world.");
        for (const std::string& rebuildChange :
             {"SceneChangeMask::Geometry", "SceneChangeMask::InstanceTopology", "SceneChangeMask::MaterialBinding"}) {
            require(level.find(rebuildChange) != std::string::npos,
                    "Every static ModelRenderer resource change must trigger a rebuild.");
        }
        require(level.find("modelRenderer_->sync(*sceneData.world") != std::string::npos,
                "ModelRenderer must consume the immutable render-world snapshot.");
        require(level.find("gi::FrameInfo{") != std::string::npos &&
                    level.find("*sceneData.world") != std::string::npos &&
                    level.find(".snapshot = sceneData.world") != std::string::npos,
                "Raster and GPU-scene GI setup must consume the same immutable render-world snapshot.");
        require(level.find("renderWorld_.sync(") == std::string::npos &&
                    level.find("scene::Level") == std::string::npos && level.find("scene::Camera") == std::string::npos,
                "Runtime must not read an active Level or Camera after the packet boundary.");
        std::cout << "RENDER_WORLD_BOUNDARY=packet>submitted-baseline>ModelRenderer+SSAO+GPUScene\n";
    }

    void verifyHybridGiIntegration(const std::string& level) {
        for (const std::string& owner : std::vector<std::string>{
                 "std::make_unique<gpu::GpuSceneResources>", "std::make_unique<gpu::GpuSceneUpdatePlanner>",
                 "std::make_unique<gi::RayTracedDirectLightingPass>", "std::make_unique<gi::SharcRadianceCache>",
                 "std::make_unique<gi::SharcIndirectLightingPass>", "std::make_unique<gi::NrdDenoiser>",
                 "std::make_unique<gi::RtDiNrdInputsPass>", "std::make_unique<gi::GiCompositePass>",
                 "std::make_unique<gi::HybridLightingCompositePass>"}) {
            require(level.find(owner) != std::string::npos, "Default pipeline is missing a hybrid GI resource owner.");
        }
        require(level.find("context_.rayTracingDecision().enabled()") != std::string::npos &&
                    level.find("supportsRayTracingPipeline()") != std::string::npos &&
                    level.find("GlobalIlluminationMode::RayTracing") != std::string::npos,
                "Hybrid RTDI selection must honor the explicit mode without requiring SHARC storage support.");
        require(level.find("LUMIN_LEVEL_RENDERER_HAS_SHARC_INDIRECT") != std::string::npos &&
                    level.find("runtime.sharc != nullptr") != std::string::npos &&
                    level.find("return gi::BackendInfo{\"Ray Tracing Direct\"") != std::string::npos,
                "Missing SHARC must retain the raw RTDI path and report its effective backend.");
        require(level.find("ensureHybridGiCapacity()") != std::string::npos &&
                    level.find("std::unique_ptr<HybridGiState> candidate = buildCandidate()") != std::string::npos,
                "Topology changes must transactionally rebuild Hybrid bindings from the waited-idle path.");

        std::size_t position = level.find("pipelines::DefaultRenderPipelineSession::addHybridSurfaceFeaturePasses");
        position = requireAfter(level, "runtime.sceneResources->recordUpdate", position);
        position = requireAfter(level, "runtime.sceneResources->candidateDescriptors", position);
        position = requireAfter(level, "runtime.sceneResources->candidateGeometry", position);
        position = requireAfter(level, "runtime.directLighting->record(", position);
        position = requireAfter(level, "runtime.sharc->record(", position);
        position = requireAfter(level, "runtime.indirectLighting->record(", position);
        position = requireAfter(level, "recordStatisticsReadback", position);
        position = requireAfter(level, "pipelines::DefaultRenderPipelineSession::addGiDenoiserFeaturePasses", position);
        position = requireAfter(level, "runtime.directNrdInputs->record(", position);
        position = requireAfter(level, "runtime.directNrd->record(", position);
        position = requireAfter(level, "runtime.directComposite->record(", position);
        position = requireAfter(level, "runtime.indirectNrd->record(", position);
        requireAfter(level, "runtime.indirectComposite->record(", position);
        require(level.find("if (runtime.sharcEnabled)") != std::string::npos &&
                    level.find("if (settings.nrdEnabled)") != std::string::npos &&
                    level.find(".reblurSettings = gi::detail::makeDirectLightingReblurSettings()") !=
                        std::string::npos &&
                    level.find("return gi::BackendInfo{\"Ray Tracing Direct + NRD\"") != std::string::npos &&
                    level.find("diffuseInput = signals.diffuseRadianceHitDistance") != std::string::npos &&
                    level.find("specularInput = signals.specularRadianceHitDistance") != std::string::npos,
                "Direct NRD must remain independent of SHARC while raw indirect signals remain valid inputs.");
        require(level.find("if (!settings.enabled)") != std::string::npos &&
                    level.find("settings.splitLambda") != std::string::npos &&
                    level.find("settings.maxDistance") != std::string::npos &&
                    level.find("settings.ssaoEnabled") != std::string::npos,
                "Legacy mode controls must drive CSM recording, cascade parameters, and SSAO.");

        require(level.find(".atmosphere = atmosphereConsumerBindingSets_[frameIndex]") != std::string::npos &&
                    level.find(".atmosphere = atmosphereData.graphRecord->resources") != std::string::npos,
                "SHARC update and indirect lighting must consume the raster atmosphere binding set and LUT graph.");
        require(level.find("currentEffectiveJitter - previousEffectiveJitter") != std::string::npos &&
                    level.find("cameraData.viewToClip = matrixElements(sceneData.camera.projection)") !=
                        std::string::npos &&
                    level.find("cameraData.worldToView = matrixElements(sceneData.camera.view)") != std::string::npos &&
                    level.find(".denoisingRange = nrdDenoisingRange") != std::string::npos,
                "NRD must receive non-jittered matrices, explicit jitter history, and the RT view-Z range.");
        require(countOccurrences(level, "gi::makeRayTracingSunIrradiance(sun, lightingSettings.enabled)") == 2,
                "RTDI and SHARC update must share the physically pre-exposed sun-irradiance conversion.");

        const std::size_t submit = level.find("context_.submitFrameCommands");
        const std::size_t runtimeCommit = level.find("commitHybridSurfaceFeature(identity)", submit);
        const std::size_t plannerCommit = level.find("runtime.scenePlanner->commit", runtimeCommit);
        const std::size_t physicalCommit = level.find("runtime.sceneResources->finishUpdate", plannerCommit);
        const std::size_t directNrdCommit = level.find("runtime.directNrd->commitSubmittedFrame", physicalCommit);
        const std::size_t directInputsCommit =
            level.find("runtime.directNrdInputs->commitSubmittedFrame", directNrdCommit);
        const std::size_t indirectNrdCommit =
            level.find("runtime.indirectNrd->commitSubmittedFrame", directInputsCommit);
        require(submit < runtimeCommit && runtimeCommit < plannerCommit && plannerCommit < physicalCommit &&
                    physicalCommit < directNrdCommit && directNrdCommit < directInputsCommit &&
                    directInputsCommit < indirectNrdCommit,
                "Hybrid candidates must publish only after queue submission succeeds.");
        require(
            level.find("runtime.sceneResources->finishUpdate(*runtime.pendingSceneUpdate, false)") !=
                    std::string::npos &&
                level.find("runtime.sharc->discardPendingFrame()") != std::string::npos &&
                level.find("runtime.indirectLighting->discardPendingFrame()") != std::string::npos &&
                level.find("runtime.directNrd->discardFrame(*runtime.pendingDirectNrdFrame)") != std::string::npos &&
                level.find("runtime.directNrdInputs->discardPendingFrame()") != std::string::npos &&
                level.find("runtime.indirectNrd->discardFrame(*runtime.pendingIndirectNrdFrame)") != std::string::npos,
            "Record and submit failures must discard every hybrid GI candidate.");
        require(level.find("DefaultRenderPipelineKind::Hybrid") != std::string::npos &&
                    level.find("rt.surface.world-position") != std::string::npos &&
                    level.find("hybridData.active = hybridPathActive") != std::string::npos,
                "Hybrid must select a dedicated RT surface topology and resource namespace.");
        std::cout << "HYBRID_GI=GPUScene>RTDI>DirectNRD>SHARC>IndirectNRD>Composite;"
                     "TRANSACTION=submit-commit/failure-discard\n";
    }

    void verifyHybridLightingResourceIsolation(const std::string& level) {
        for (const std::string& token : {
                 ".directRadiance = postFx.directRadiance.texture",
                 "\"rt.surface.direct-radiance\"",
                 "\"rt.direct-radiance.denoised\"",
                 "\"global-illumination.output\"",
                 "\"lighting.hdr\"",
                 ".directRadiance = lighting.direct.texture",
                 ".indirectRadiance = lighting.combined.texture",
                 ".output = sceneHdr.color.texture",
             }) {
            require(level.find(token) != std::string::npos,
                    "Hybrid lighting must preserve distinct raw direct, denoised direct, indirect, and HDR resources.");
        }
        require(level.find(".directRadiance = postFx.globalIllumination.texture") == std::string::npos,
                "RTDI must never alias the SHARC/NRD indirect-lighting target.");
        for (const std::string& state : {"directRadianceInitialized_", "directNrdOutputInitialized_",
                                         "globalIlluminationInitialized_", "frameSlotInitialized(frameIndex)"}) {
            require(level.find(state) != std::string::npos,
                    "Hybrid-only textures must publish their initial state independently after queue submission.");
        }

        const std::string lightingComposite = readSource("render/gi/raytracing/HybridLightingComposite.cpp");
        const std::string lightingCompositeShader = readSource("shaders/HybridLightingComposite.slang");
        require(lightingComposite.find("mode != HybridLightingCompositeMode::DirectOnly") != std::string::npos &&
                    lightingCompositeShader.find("float4 packedIndirect =") == std::string::npos,
                "Direct-only composition must not transition or load an uninitialized indirect texture.");

        const std::size_t synchronize =
            level.find("void pipelines::DefaultRenderPipelineSession::synchronizeRenderConfiguration");
        const std::size_t configuration =
            level.find("pipelines::DefaultRenderPipelineSession::FeatureConfigurationState", synchronize);
        require(synchronize != std::string::npos && configuration != std::string::npos,
                "Hybrid settings synchronization source is missing.");
        const std::string synchronizationBody = level.substr(synchronize, configuration - synchronize);
        require(synchronizationBody.find("hybridGi_->sharcEnabled = requestedSharcRuntime") != std::string::npos &&
                    synchronizationBody.find("createHybridGiResources()") == std::string::npos,
                "SHARC toggles must switch the resident branch without rebuilding Hybrid resources.");
        std::cout << "HYBRID_LIGHTING_RESOURCES=raw-direct!=denoised-direct!=indirect!=HDR;"
                     "SHARC_TOGGLE=resident\n";
    }

    void verifyFeaturePipelineContract(const std::string& level, const std::string& pipelineDefinition,
                                       const std::string& frameContracts) {
        for (const std::string& history :
             {"HistoryDomain::Taa", "HistoryDomain::NrdDiffuse", "HistoryDomain::NrdSpecular", "HistoryDomain::Sharc",
              "HistoryDomain::AtmosphereLut"}) {
            require(pipelineDefinition.find(history) != std::string::npos,
                    "Default Feature recipes must assign every independent history domain.");
        }
        require(
            level.find("RenderPipelineRecipeResolver::resolve") != std::string::npos &&
                level.find("std::make_unique<core::RenderPipelineInstance>") != std::string::npos &&
                level.find("candidate->onRenderExtentChanged") != std::string::npos,
            "Runtime must resolve a recipe and initialize a complete candidate before replacing the active instance.");
        for (const std::string& contractType :
             {"FrameSceneData", "GpuSceneData", "RasterSurfaceData", "RtSurfaceData", "AtmosphereData",
              "IndirectLightingData", "DenoisedLightingData", "SceneHdrData", "TemporalOutputData", "PresentData"}) {
            require(frameContracts.find(contractType) != std::string::npos,
                    "RenderCore must expose each typed frame-data contract.");
        }
        std::cout << "FEATURE_PIPELINE=typed-DAG>candidate-instance>transactional-swap\n";
    }

    void verifySwapchainLifecycle(const std::string& context) {
        require(context.find("window_") == std::string::npos &&
                    context.find("void VulkanContext::updateSurfaceState") != std::string::npos,
                "VulkanContext must discard Window after surface bootstrap and consume value SurfaceState updates.");
        require(context.find("swapchainTextureInitialized_") != std::string::npos,
                "Swapchain wrapper initialization state is not tracked.");
        require(context.find("ResourceStates::Unknown") != std::string::npos,
                "New swapchain wrappers must begin in Unknown state.");
        require(context.find("ResourceStates::Present") != std::string::npos,
                "Successful swapchain use must transition to Present.");
        const std::size_t wrappers = context.find("swapchainTextures_.clear()");
        const std::size_t views = context.find("vkDestroyImageView", wrappers);
        const std::size_t nativeSwapchain = context.find("vkDestroySwapchainKHR", wrappers);
        require(wrappers < views && views < nativeSwapchain,
                "Swapchain wrappers must be released before native image views and swapchain.");
        const std::size_t beginFrame = context.find("std::optional<VulkanFrame> VulkanContext::beginFrame()");
        const std::size_t garbageCollection = context.find("rhiDevice_->runGarbageCollection()", beginFrame);
        const std::size_t acquire = context.find("vkAcquireNextImageKHR", beginFrame);
        require(beginFrame != std::string::npos && garbageCollection != std::string::npos &&
                    acquire != std::string::npos && beginFrame < garbageCollection && garbageCollection < acquire,
                "Each frame must retire completed NvRHI command buffers before acquiring and recording new work.");
        std::size_t cancellation = context.find("void VulkanContext::cancelFrame");
        cancellation = requireAfter(context, "frame.commandList->close()", cancellation);
        cancellation = requireAfter(context, "setEnableAutomaticBarriers(false)", cancellation);
        cancellation = requireAfter(context, "queueWaitForSemaphore", cancellation);
        cancellation = requireAfter(context, "executeCommandLists", cancellation);
        cancellation = requireAfter(context, "waitIdle()", cancellation);
        requireAfter(context, "recreateSwapchain()", cancellation);
        std::cout << "SWAPCHAIN_STATES=Unknown>Present;RECREATE=Unknown;WRAPPER_RELEASE=before-native\n";
    }

    void verifySynchronousRuntime(const std::string& renderer) {
        require(renderer.find("std::thread") == std::string::npos &&
                    renderer.find("RenderMailbox") == std::string::npos &&
                    renderer.find("session->drawFrame(std::move(packet), ui)") != std::string::npos,
                "Renderer must draw synchronously on the SDL/Vulkan owning main thread.");
        require(renderer.find("session.reset();") < renderer.find("context.reset();"),
                "Feature Runtime resources must be destroyed before VulkanContext on the owning thread.");
        for (const std::string& forbidden : {"scene::Level", "scene::Camera", "ImGui::", "SDL_"}) {
            require(renderer.find(forbidden) == std::string::npos,
                    "Renderer must consume values without reaching into active logic or global framework state.");
        }
        std::cout << "SYNC_RUNTIME=direct-draw;GPU_OWNER=os-main-thread\n";
    }

    void verifyNvrhiYCoordinateConvention(const std::string& level) {
        const std::string camera = readSource("core/scene/Camera.cpp");
        require(camera.find("projection[1][1] *= -1.0f") == std::string::npos,
                "Camera projection must not duplicate NvRHI Vulkan's viewport Y inversion.");
        require(level.find("lightProjection[1][1] *= -1.0f") == std::string::npos,
                "Cascade projections must not duplicate NvRHI Vulkan's viewport Y inversion.");
        const std::string jitterY = "projection[2][1] += (2.0f * jitter.y) / static_cast<float>(height)";
        require(level.find(jitterY) != std::string::npos,
                "TAA jitter Y must retain its positive logical-framebuffer convention.");

        const std::string fullscreenPosition =
            "output.position = float4(triangleUv.x * 2.0 - 1.0, 1.0 - triangleUv.y * 2.0, 0.0, 1.0);";
        for (const std::string& path : {"shaders/Deferred.slang", "shaders/ao/AoCommon.slang", "shaders/Sky.slang",
                                        "shaders/Taa.slang", "shaders/Bloom.slang", "shaders/PostProcess.slang"}) {
            require(readSource(path).find(fullscreenPosition) != std::string::npos,
                    path + " must map logical top UV to positive clip-space Y.");
        }

        const std::string gbuffer = readSource("shaders/GBuffer.slang");
        require(gbuffer.find("return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);") != std::string::npos,
                "G-buffer motion must convert positive NDC Y toward smaller screen V.");
        const std::string sky = readSource("shaders/Sky.slang");
        require(sky.find("float2 clipPosition = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);") !=
                    std::string::npos,
                "Sky inverse projection must reconstruct the NvRHI clip-space Y convention.");
        const std::string deferred = readSource("shaders/Deferred.slang");
        require(deferred.find("float2 shadowUv = float2(shadowNdc.x * 0.5 + 0.5, 0.5 - shadowNdc.y * 0.5);") !=
                    std::string::npos,
                "Deferred shadows must convert positive light NDC Y toward smaller shadow V.");
        std::cout << "NVRHI_Y_CONVENTION=projection,fullscreen,motion,sky,shadow\n";
    }

    void verifyHybridMotionContract() {
        const std::string rtDirect = readSource("shaders/RtDi.slang");
        require(rtDirect.find("float2 currentUv = (float2(pixel) + 0.5) / float2(extent);") != std::string::npos &&
                    rtDirect.find("float2 currentUv = (float2(pixel) + 0.5) / float2(extent) +") == std::string::npos,
                "RTDI current UV must use the jittered dispatch sample exactly once.");

        const std::string indirect = readSource("shaders/SharcIndirectLighting.slang");
        require(
            indirect.find("denoiserMotion[pixel] = motionTexture.Load(int3(pixel, 0)) + frame.renderParameters.zw;") !=
                std::string::npos,
            "SHARC indirect must remove projection jitter before handing motion to NRD.");
        const std::string directNrd = readSource("shaders/RtDiNrdInputs.slang");
        require(
            directNrd.find("denoiserMotion[pixel] = motionTexture.Load(int3(pixel, 0)) + frame.renderParameters.xy;") !=
                std::string::npos,
            "Direct NRD must remove projection jitter independently of SHARC.");
        std::cout << "HYBRID_MOTION=RTDI-jitter-once;NRD=non-jittered-previous-minus-current\n";
    }

    void verifyStableTaaContract(const std::string& level) {
        const std::string taa = readSource("shaders/Taa.slang");
        require(taa.find("[[vk::binding(0, 0)]] Texture2D<float4> positionTexture;") != std::string::npos &&
                    level.find("sceneHdr.position = hybridPathActive ? rtSurface.worldPositionHitDistance : ") !=
                        std::string::npos &&
                    level.find("sceneHdr.position.graphResource") != std::string::npos,
                "TAA must consume the active Raster/Hybrid position signal through the FrameGraph.");
        require(
            taa.find("float2 jitteredUv = stableUv + frame.temporalOptions.xy;") != std::string::npos &&
                taa.find("float2 stableMotion = rawMotion + frame.temporalOptions.xy - frame.temporalOptions.zw;") !=
                    std::string::npos &&
                taa.find("float2 previousUv = input.uv + stableMotion;") != std::string::npos,
            "TAA must reconstruct current geometry and history in stable screen UVs.");
        require(level.find("postProcess.uniforms.inverseViewProjection = glm::inverse(viewProjection)") !=
                        std::string::npos &&
                    taa.find("lightingTexture.SampleLevel(renderSampler, jitteredUv, 0.0)") != std::string::npos &&
                    taa.find("if (!geometry)") != std::string::npos,
                "TAA must stabilize the jittered sky while rejecting its unavailable history motion.");
        require(level.find("action != core::HistoryAction::Keep") != std::string::npos &&
                    level.find("postProcess.uniforms.temporalOptions.z = postProcess.uniforms.temporalOptions.x") !=
                        std::string::npos,
                "TAA history invalidation must reset the previous jitter coordinate baseline.");
        std::cout << "TAA_COORDINATES=stable;MOTION=de-jittered;BACKGROUND=stabilized-current-only\n";
    }

    void verifyFsr1RcasIntegration(const std::string& level) {
        const std::string postProcess = readSource("shaders/PostProcess.slang");
        const std::string rcas = readSource("shaders/include/Fsr1Rcas.slang");
        require(postProcess.find("import Fsr1Rcas;") != std::string::npos &&
                    postProcess.find("frame.renderOptions.w >= 0.5 && frame.tonemapOptions.z > 0.0") !=
                        std::string::npos &&
                    postProcess.find("mapped = luminFsr1Rcas(") != std::string::npos,
                "FSR1 RCAS must sharpen only enabled TAA output in the final linear display domain.");
        require(rcas.find("if (sharpness <= 0.0)") != std::string::npos &&
                    rcas.find("noise = -0.5 * noise + 1.0") != std::string::npos &&
                    rcas.find("exp2(-2.0 * (1.0 - saturate(sharpness)))") != std::string::npos,
                "FSR1 RCAS must preserve zero-sharpness identity, denoising, and Alpha-Piscium strength mapping.");
        require(level.find("frame.history.texture, nvrhi::TextureSlice{}, frame.taaResolved.texture") !=
                    std::string::npos,
                "TAA history must copy the unsharpened resolve so RCAS cannot accumulate edge halos.");
        std::cout << "TAA_SHARPEN=FSR1-RCAS;DOMAIN=post-tonemap-linear;HISTORY=unsharpened\n";
    }

    void verifyBloomAndAgxIntegration(const std::string& level) {
        const std::string bloom = readSource("shaders/Bloom.slang");
        const std::string postProcess = readSource("shaders/PostProcess.slang");
        require(bloom.find("float3 downsample13Tap") != std::string::npos &&
                    bloom.find("(a + b + c + d) * 0.125") != std::string::npos &&
                    bloom.find("float3 upsampleTent") != std::string::npos &&
                    bloom.find("result += sourceTexture.SampleLevel") != std::string::npos,
                "Bloom must retain the Alpha-Piscium 13-tap downsample and tent upsample filters.");
        require(level.find("bloomLevelCount = 6") != std::string::npos ||
                    readSource("render/features/postfx/PostFxResources.hpp").find("bloomLevelCount = 6") !=
                        std::string::npos,
                "Bloom must keep a six-level HDR downsample pyramid.");
        require(level.find("\"Bloom bypass\", FrameGraphPassType::Transfer") != std::string::npos &&
                    level.find("\"Bloom downsample \"") != std::string::npos &&
                    level.find("\"Bloom upsample \"") != std::string::npos &&
                    level.find("\"Bloom composite\", FrameGraphPassType::Graphics") != std::string::npos,
                "Bloom must expose bypass, downsample, upsample, and composite FrameGraph passes.");
        require(postProcess.find("[[vk::binding(14, 0)]] Texture2D<float4> bloomTexture") != std::string::npos &&
                    postProcess.find("float3 agxInset") != std::string::npos &&
                    postProcess.find("agxDefaultContrastApprox") != std::string::npos &&
                    postProcess.find("frame.tonemapOptions.w >= 0.5 ? agxDisplayLinear(exposed) : acesFilm(exposed)") !=
                        std::string::npos,
                "Tone Mapping must read Bloom HDR and switch between AgX and the ACES fallback.");
        std::cout << "BLOOM=6-down,5-up,composite;TONEMAP=AgX-or-ACES\n";
    }

    void verifyHybridRaySidednessContract() {
        const std::string gpuScene = readSource("shaders/include/GpuScene.slang");
        require(gpuScene.find("float3 luminOrientShadingNormal") != std::string::npos,
                "GPU Scene shaders must provide one shared double-sided normal orientation helper.");

        for (const std::string& path :
             {"shaders/RtDi.slang", "shaders/SharcIndirectLighting.slang", "shaders/SharcUpdate.slang"}) {
            const std::string source = readSource(path);
            require(source.find("RAY_FLAG_CULL_BACK_FACING_TRIANGLES") == std::string::npos,
                    path + " must not cull triangles that the raster G-buffer renders.");
            require(source.find("luminOrientShadingNormal") != std::string::npos,
                    path + " must orient radiance-hit normals against the incoming ray.");
        }
        std::cout << "HYBRID_SIDEDNESS=double-sided;normal=against-incoming-ray\n";
    }

} // namespace

int main() {
    try {
        const std::string level = readSource("render/pipelines/default/DefaultRenderPipelineSession.cpp") +
                                  readSource("render/pipelines/default/DefaultFeatureRegistry.cpp") +
                                  readSource("render/pipelines/default/DefaultRenderPipelineFrame.cpp") +
                                  readSource("render/pipelines/default/DefaultRenderResources.cpp") +
                                  readSource("render/pipelines/default/DefaultRenderFeatures.cpp");
        const std::string context = readSource("render/platform/vulkan/VulkanContext.cpp");
        const std::string renderer = readSource("render/runtime/Renderer.cpp");
        const std::string pipelineDefinition = readSource("render/pipelines/DefaultRenderPipelines.cpp");
        const std::string frameContracts = readSource("render/core/FrameDataContracts.hpp");
        verifyPassOrder(level, pipelineDefinition);
        verifyNvrhiRecording(level);
        verifyHistoryAndErrorPaths(level);
        verifyRenderWorldSnapshotBoundary(level);
        verifyHybridGiIntegration(level);
        verifyHybridLightingResourceIsolation(level);
        verifyFeaturePipelineContract(level, pipelineDefinition, frameContracts);
        verifySwapchainLifecycle(context);
        verifySynchronousRuntime(renderer);
        verifyNvrhiYCoordinateConvention(level);
        verifyHybridMotionContract();
        verifyStableTaaContract(level);
        verifyFsr1RcasIntegration(level);
        verifyBloomAndAgxIntegration(level);
        verifyHybridRaySidednessContract();
        std::cout << "DEFAULT_PIPELINE_SESSION=PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DEFAULT_PIPELINE_SESSION=FAIL: " << exception.what() << '\n';
        return 1;
    }
}

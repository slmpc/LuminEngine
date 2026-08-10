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

    void verifyPassOrder(const std::string& level, const std::string& deferredPipeline) {
        std::size_t position = level.find("LevelRenderer::recordCommandList");
        require(position != std::string::npos, "NvRHI frame recorder entry point is missing.");
        for (const std::string& pass : std::vector<std::string>{
                 "CSM cascade ", "G-buffer", "globalIllumination_->addPasses", "Procedural sky", "Deferred lighting",
                 "TAA resolve", "TAA history copy", "TAA history ready", "Tonemap", "ImGui overlay", "Present"}) {
            position = requireAfter(level, pass, position);
        }
        require(level.find("builder.readTexture(data.globalIllumination") != std::string::npos,
                "Sky must read the GI output to preserve its FrameGraph dependency.");
        require(level.find("builder.readTexture(data.historyWrite") != std::string::npos,
                "Tonemap must read historyWrite to preserve history-ready ordering.");

        position = 0;
        for (const std::string& feature :
             {"feature(\"shadow\"", "feature(\"gbuffer\"", "feature(\"atmosphere-luts\"",
              "feature(\"global-illumination\"", "feature(\"gi-denoiser\"", "feature(\"sky-composite\"",
              "feature(\"direct-lighting\"", "feature(\"temporal-aa\"", "feature(\"tone-mapping\"",
              "feature(\"ui-present\""}) {
            position = requireAfter(deferredPipeline, feature, position);
        }
        std::cout << "PASS_ORDER=CSMx4>G-buffer>GI>sky>deferred>TAA>history-copy>history-ready>tonemap>ImGui>Present\n";
    }

    void verifyNvrhiRecording(const std::string& level) {
        require(level.find("frame->commandList") != std::string::npos,
                "drawFrame must use the active NvRHI command list.");
        require(level.find("copyTexture") != std::string::npos, "TAA history must be copied with copyTexture.");
        require(countOccurrences(level, "copyTexture") == 1, "Exactly one TAA history copy must be recorded.");
        require(level.find("ResourceStates::CopySource") != std::string::npos,
                "TAA history source must be declared CopySource.");
        require(level.find("ResourceStates::CopyDest") != std::string::npos,
                "TAA history destination must be declared CopyDest.");
        require(level.find("context_.modelRendererCapabilities()") != std::string::npos,
                "LevelRenderer must hand explicit device-derived ModelRendererCapabilities to ModelRenderer.");
        require(level.find("textureDesc(frame.materialId") != std::string::npos &&
                    level.find("addColorAttachment(frame.materialId.texture)") != std::string::npos &&
                    level.find("clearTextureUInt(data.frame->materialId.texture") != std::string::npos &&
                    level.find("GpuMaterialIndex::invalidValue") != std::string::npos,
                "The G-buffer must own, attach, and integer-clear a dedicated stable material-ID texture.");
        require(level.find("modelRenderer_->materialBufferInitialState(frameIndex)") != std::string::npos &&
                    level.find("builder.read(data.materials, nvrhi::ResourceStates::ShaderResource)") !=
                        std::string::npos,
                "Direct lighting must import and declare the frame-slot material buffer through FrameGraph.");
        require(level.find("setRegisterSpaceAndDescriptorSet(1)") != std::string::npos &&
                    level.find("addBindingSet(directLightingBindingSets_[frameIndex])") != std::string::npos,
                "Deferred direct lighting must bind its material resources in descriptor set 1.");
        require(level.find("? nvrhi::ResourceStates::DepthWrite") != std::string::npos &&
                    level.find("textureDesc(frame.depth, depthInitialState)") != std::string::npos,
                "The non-sampled G-buffer depth texture must retain DepthWrite across frame-slot reuse.");
        for (const std::string& pass : {"CSM clear", "G-buffer clear", "Procedural sky clear", "TAA clear"}) {
            const std::size_t name = level.find(pass);
            require(name != std::string::npos, "Every runtime attachment clear needs a dedicated FrameGraph pass.");
            const std::size_t copyDest = level.find("ResourceStates::CopyDest", name);
            const std::size_t addPassEnd = level.find("graph.addPass", name + 1);
            require(copyDest != std::string::npos && (addPassEnd == std::string::npos || copyDest < addPassEnd),
                    "Runtime clear passes must declare their textures as CopyDest.");
        }
        require(level.find("Tonemap clear") == std::string::npos,
                "Swapchain images do not guarantee transfer-destination usage and must not be transfer-cleared.");
        const std::size_t tonemap = level.find("\"Tonemap\", FrameGraphPassType::Graphics");
        const std::size_t tonemapRenderTarget =
            level.find("builder.writeTexture(data.swap, nvrhi::ResourceStates::RenderTarget)", tonemap);
        require(tonemap != std::string::npos && tonemapRenderTarget != std::string::npos,
                "Tonemap must cover the swapchain directly as a render target.");
        for (const std::string& forbidden :
             std::vector<std::string>{"vkCmd", "VkRendering", "VkDescriptorSet", "VkPipeline", "VkCommandBuffer"}) {
            require(level.find(forbidden) == std::string::npos,
                    std::string{"Forbidden direct Vulkan recorder token remains: "} + std::string{forbidden});
        }
        std::cout << "TAA_COPY_COUNT=1\nMODEL_CAPABILITIES=explicit-device-derived\n";
    }

    void verifyHistoryAndErrorPaths(const std::string& level) {
        for (const std::string& token : std::vector<std::string>{
                 "pendingFrameChanges_.merge(core::frameChangesFromScene(sceneDelta.changes))",
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
        require(level.find("callbacks.gbuffer.onSubmitted") != std::string::npos &&
                    level.find("modelRenderer_->commitSubmittedFrame") != std::string::npos &&
                    level.find("callbacks.temporalAa.onSubmitted") != std::string::npos &&
                    level.find("textures_.markHistoryValid") != std::string::npos,
                "Model and TAA histories must be committed through Feature submission callbacks.");
        require(level.find("modelRenderer_->discardPendingFrame") != std::string::npos,
                "Discarded Feature frames must abandon pending model transforms.");
        require(level.find("textures_.invalidateHistory") == std::string::npos &&
                    level.find("gi::shouldInvalidateHistory") == std::string::npos,
                "LevelRenderer must not retain ad hoc TAA or GI invalidation decisions.");
        require(countOccurrences(level, "renderPipeline_->discardFrame") >= 3,
                "Record, submit and swapchain-rebuild paths must all discard pending Feature work.");
        require(level.find("context_.cancelFrame") != std::string::npos,
                "Record failure must cancel the acquired frame through VulkanContext.");
        require(level.find("imgui_.cancelFrame") != std::string::npos,
                "Acquire/record failures must cancel the prepared UI frame.");
        std::cout << "HISTORY_COORDINATION=FrameChangeSet>prepare>submit>commit;SEQUENCE=success-only\n"
                     "ERROR_PATH=acquire/record/submit-discard,retry\n";
    }

    void verifyRenderWorldSnapshotBoundary(const std::string& level) {
        require(level.find("renderWorld_.sync(level_)") != std::string::npos,
                "LevelRenderer must synchronize its renderer-owned world before recording.");
        for (const std::string& rebuildChange :
             {"SceneChangeMask::Geometry", "SceneChangeMask::InstanceTopology", "SceneChangeMask::MaterialBinding"}) {
            require(level.find(rebuildChange) != std::string::npos,
                    "Every static ModelRenderer resource change must trigger a rebuild.");
        }
        require(level.find("modelRenderer_->sync(*data.renderWorld") != std::string::npos,
                "ModelRenderer must consume the immutable render-world snapshot.");
        require(level.find("gi::FrameInfo{") != std::string::npos &&
                    level.find("*data.renderWorld") != std::string::npos &&
                    level.find(".snapshot = data.renderWorldSnapshot") != std::string::npos,
                "Raster and GPU-scene GI setup must consume the same immutable render-world snapshot.");
        require(level.find("modelRenderer_->sync(level_") == std::string::npos &&
                    level.find("gi::FrameInfo{level_") == std::string::npos,
                "Rendering features must not read the live Level after snapshot extraction.");
        std::cout << "RENDER_WORLD_BOUNDARY=sync>snapshot>ModelRenderer+SSAO+GPUScene\n";
    }

    void verifyHybridGiIntegration(const std::string& level) {
        for (const std::string& owner : std::vector<std::string>{
                 "std::make_unique<gpu::GpuSceneResources>", "std::make_unique<gpu::GpuSceneUpdatePlanner>",
                 "std::make_unique<gi::RayTracedDirectLightingPass>", "std::make_unique<gi::SharcRadianceCache>",
                 "std::make_unique<gi::RayTracedGiPass>", "std::make_unique<gi::NrdDenoiser>",
                 "std::make_unique<gi::GiCompositePass>", "std::make_unique<gi::HybridLightingCompositePass>"}) {
            require(level.find(owner) != std::string::npos, "LevelRenderer is missing a hybrid GI resource owner.");
        }
        require(level.find("context_.rayTracingDecision().enabled()") != std::string::npos &&
                    level.find("supportsSharcShaderStorage()") != std::string::npos &&
                    level.find("GlobalIlluminationMode::Ssao") != std::string::npos,
                "Hybrid GI selection must honor runtime capability and explicit SSAO fallback mode.");
        require(level.find("requiredCapacity > hybridGi_->geometryDescriptorCapacity") != std::string::npos &&
                    level.find("ensureHybridGiCapacity()") != std::string::npos,
                "Growing geometry descriptor arrays must be rebuilt from the waited-idle topology path.");

        std::size_t position = level.find("LevelRenderer::addHybridSurfaceFeaturePasses");
        position = requireAfter(level, "runtime.sceneResources->recordUpdate", position);
        position = requireAfter(level, "runtime.sceneResources->candidateDescriptors", position);
        position = requireAfter(level, "runtime.sceneResources->candidateGeometry", position);
        position = requireAfter(level, "runtime.sharc->record(", position);
        position = requireAfter(level, "runtime.rayTracedGi->record(", position);
        position = requireAfter(level, "recordStatisticsReadback", position);
        position = requireAfter(level, "LevelRenderer::addGiDenoiserFeaturePasses", position);
        position = requireAfter(level, "runtime.nrd->record(", position);
        requireAfter(level, "runtime.composite->record(", position);

        require(level.find(".atmosphere = atmosphereConsumerBindingSets_[data.frameIndex]") != std::string::npos &&
                    level.find(".atmosphere = data.atmosphereLuts->resources") != std::string::npos,
                "RT GI and SHARC must consume the raster atmosphere binding set and the same LUT graph resources.");
        require(level.find("currentEffectiveJitter - previousEffectiveJitter") != std::string::npos &&
                    level.find("cameraData.viewToClip = matrixElements(data.projection)") != std::string::npos &&
                    level.find("cameraData.worldToView = matrixElements(data.view)") != std::string::npos &&
                    level.find(".denoisingRange = nrdDenoisingRange") != std::string::npos,
                "NRD must receive non-jittered matrices, explicit jitter history, and the RT view-Z range.");
        require(level.find("return glm::vec4{sun.color * directScale, 1.0f}") != std::string::npos,
                "Disabling physical atmosphere must preserve the shared procedural environment fallback.");

        const std::size_t submit = level.find("context_.submitFrameCommands");
        const std::size_t runtimeCommit = level.find("commitSubmittedRuntimeFrame(identity)", submit);
        const std::size_t plannerCommit = level.find("runtime.scenePlanner->commit", runtimeCommit);
        const std::size_t physicalCommit = level.find("runtime.sceneResources->finishUpdate", plannerCommit);
        const std::size_t nrdCommit = level.find("runtime.nrd->commitSubmittedFrame", physicalCommit);
        require(submit < runtimeCommit && runtimeCommit < plannerCommit && plannerCommit < physicalCommit &&
                    physicalCommit < nrdCommit,
                "Hybrid candidates must publish only after queue submission succeeds.");
        require(level.find("runtime.sceneResources->finishUpdate(*runtime.pendingSceneUpdate, false)") !=
                        std::string::npos &&
                    level.find("runtime.sharc->discardPendingFrame()") != std::string::npos &&
                    level.find("runtime.rayTracedGi->discardPendingFrame()") != std::string::npos &&
                    level.find("runtime.nrd->discardFrame(*runtime.pendingNrdFrame)") != std::string::npos,
                "Record and submit failures must discard every hybrid GI candidate.");
        require(level.find("DeferredRenderPath::Hybrid") != std::string::npos &&
                    level.find("rt.surface.world-position") != std::string::npos &&
                    level.find("if (!data.hybridPathActive)") != std::string::npos,
                "Hybrid must select a dedicated RT surface topology and resource namespace.");
        std::cout << "HYBRID_GI=GPUScene>RTDI>SHARC>RT>NRD>Composite;TRANSACTION=submit-commit/failure-discard\n";
    }

    void verifyFeaturePipelineContract(const std::string& level, const std::string& deferredPipeline,
                                       const std::string& deferredHeader) {
        for (const std::string& history :
             {"HistoryDomain::Taa", "HistoryDomain::NrdDiffuse", "HistoryDomain::NrdSpecular", "HistoryDomain::Sharc",
              "HistoryDomain::AtmosphereLut"}) {
            require(deferredPipeline.find(history) != std::string::npos,
                    "Deferred Feature planning must assign every independent history domain.");
        }
        require(deferredPipeline.find("pipeline_.prepareFrame") != std::string::npos &&
                    deferredPipeline.find("pipeline_.commitFrame") != std::string::npos &&
                    deferredPipeline.find("pipeline_.discardFrame") != std::string::npos,
                "Deferred rendering must delegate every frame transaction to RenderFeaturePipeline.");
        require(level.find("blackboard.set(AtmosphereInvalidationSignatures") != std::string::npos &&
                    deferredHeader.find("std::uint64_t optical") != std::string::npos &&
                    deferredHeader.find("std::uint64_t lighting") != std::string::npos &&
                    deferredHeader.find("std::uint64_t view") != std::string::npos,
                "Atmosphere Feature must reserve independent optical, lighting and view signatures.");
        std::cout
            << "FEATURE_PIPELINE=shadow>gbuffer>atmosphere-LUT>GI>NRD>sky/composite>direct>TAA>tonemap>UI/present\n";
    }

    void verifySwapchainLifecycle(const std::string& context) {
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
        std::size_t cancellation = context.find("void VulkanContext::cancelFrame");
        cancellation = requireAfter(context, "frame.commandList->close()", cancellation);
        cancellation = requireAfter(context, "setEnableAutomaticBarriers(false)", cancellation);
        cancellation = requireAfter(context, "queueWaitForSemaphore", cancellation);
        cancellation = requireAfter(context, "executeCommandLists", cancellation);
        cancellation = requireAfter(context, "waitIdle()", cancellation);
        requireAfter(context, "recreateSwapchain()", cancellation);
        std::cout << "SWAPCHAIN_STATES=Unknown>Present;RECREATE=Unknown;WRAPPER_RELEASE=before-native\n";
    }

    void verifyNvrhiYCoordinateConvention(const std::string& level) {
        const std::string camera = readSource("src/scene/Camera.cpp");
        require(camera.find("projection[1][1] *= -1.0f") == std::string::npos,
                "Camera projection must not duplicate NvRHI Vulkan's viewport Y inversion.");
        require(level.find("lightProjection[1][1] *= -1.0f") == std::string::npos,
                "Cascade projections must not duplicate NvRHI Vulkan's viewport Y inversion.");
        const std::string jitterY = "projection[2][1] += (2.0f * jitter.y) / static_cast<float>(height)";
        require(level.find(jitterY) != std::string::npos,
                "TAA jitter Y must retain its positive logical-framebuffer convention.");

        const std::string fullscreenPosition =
            "output.position = float4(triangleUv.x * 2.0 - 1.0, 1.0 - triangleUv.y * 2.0, 0.0, 1.0);";
        for (const std::string& path : {"shaders/deferred.slang", "shaders/ssao.slang", "shaders/sky.slang",
                                        "shaders/taa.slang", "shaders/postprocess.slang"}) {
            require(readSource(path).find(fullscreenPosition) != std::string::npos,
                    path + " must map logical top UV to positive clip-space Y.");
        }

        const std::string gbuffer = readSource("shaders/gbuffer.slang");
        require(gbuffer.find("return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);") != std::string::npos,
                "G-buffer motion must convert positive NDC Y toward smaller screen V.");
        const std::string sky = readSource("shaders/sky.slang");
        require(sky.find("float2 clipPosition = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);") !=
                    std::string::npos,
                "Sky inverse projection must reconstruct the NvRHI clip-space Y convention.");
        const std::string deferred = readSource("shaders/deferred.slang");
        require(deferred.find("float2 shadowUv = float2(shadowNdc.x * 0.5 + 0.5, 0.5 - shadowNdc.y * 0.5);") !=
                    std::string::npos,
                "Deferred shadows must convert positive light NDC Y toward smaller shadow V.");
        std::cout << "NVRHI_Y_CONVENTION=projection,fullscreen,motion,sky,shadow\n";
    }

} // namespace

int main() {
    try {
        const std::string level = readSource("src/render/LevelRenderer.cpp");
        const std::string context = readSource("src/render/VulkanContext.cpp");
        const std::string deferredPipeline = readSource("src/render/DeferredRenderPipeline.cpp");
        const std::string deferredHeader = readSource("src/render/DeferredRenderPipeline.hpp");
        verifyPassOrder(level, deferredPipeline);
        verifyNvrhiRecording(level);
        verifyHistoryAndErrorPaths(level);
        verifyRenderWorldSnapshotBoundary(level);
        verifyHybridGiIntegration(level);
        verifyFeaturePipelineContract(level, deferredPipeline, deferredHeader);
        verifySwapchainLifecycle(context);
        verifyNvrhiYCoordinateConvention(level);
        std::cout << "LEVEL_RENDERER_RECORDER=PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "LEVEL_RENDERER_RECORDER=FAIL: " << exception.what() << '\n';
        return 1;
    }
}

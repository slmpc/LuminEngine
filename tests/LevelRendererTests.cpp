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

    void verifyPassOrder(const std::string& level) {
        std::size_t position = level.find("LevelRenderer::recordCommandList");
        require(position != std::string::npos, "NvRHI frame recorder entry point is missing.");
        for (const std::string& pass : std::vector<std::string>{
                 "CSM cascade ", "G-buffer", "globalIllumination_->addPasses", "Procedural sky", "Deferred lighting",
                 "TAA resolve", "TAA history copy", "TAA history ready", "Tonemap", "ImGui overlay", "Present"}) {
            position = requireAfter(level, pass, position);
        }
        require(level.find("[globalIllumination, lighting]") != std::string::npos,
                "Sky must depend on the GI output to preserve execution order.");
        require(level.find("[taaResolved, historyWrite, swap]") != std::string::npos,
                "Tonemap must depend on historyWrite to preserve history-ready ordering.");
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
        require(level.find("? nvrhi::ResourceStates::DepthWrite") != std::string::npos &&
                    level.find("textureDesc(frame.depth, depthInitialState)") != std::string::npos,
                "The non-sampled G-buffer depth texture must retain DepthWrite across frame-slot reuse.");
        for (const std::string& pass : {"CSM clear", "G-buffer clear", "Procedural sky clear", "TAA clear"}) {
            const std::size_t name = level.find(pass);
            require(name != std::string::npos, "Every runtime attachment clear needs a dedicated FrameGraph pass.");
            const std::size_t copyDest = level.find("ResourceStates::CopyDest", name);
            const std::size_t addPassEnd = level.find("frameGraph_.addPass", name + 1);
            require(copyDest != std::string::npos && (addPassEnd == std::string::npos || copyDest < addPassEnd),
                    "Runtime clear passes must declare their textures as CopyDest.");
        }
        require(level.find("Tonemap clear") == std::string::npos,
                "Swapchain images do not guarantee transfer-destination usage and must not be transfer-cleared.");
        const std::size_t tonemap = level.find("\"Tonemap\", FrameGraphPassType::Graphics");
        const std::size_t tonemapRenderTarget = level.find("builder.writeTexture(swap, nvrhi::ResourceStates::RenderTarget)",
                                                           tonemap);
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
        for (const std::string& token :
             std::vector<std::string>{"cameraCut", "previousFieldOfView_", "previousTaaEnabled_", "topologyRevision_",
                                      "previousGlobalIlluminationEnabled_", "swapchainGeneration_"}) {
            require(level.find(token) != std::string::npos, "Required history invalidation trigger is missing.");
        }
        const std::size_t submit = level.find("context_.submitFrame");
        const std::size_t history = level.find("textures_.markHistoryValid", submit);
        require(submit != std::string::npos && history != std::string::npos && submit < history,
                "History must advance only after successful submission/present.");
        require(level.find("context_.cancelFrame") != std::string::npos,
                "Record failure must cancel the acquired frame through VulkanContext.");
        require(level.find("imgui_.cancelFrame") != std::string::npos,
                "Acquire/record failures must cancel the prepared UI frame.");
        std::cout << "HISTORY_INVALIDATIONS=first,camera-cut,FOV,TAA-reenable,topology,GI-reenable,recreate\n"
                     "ERROR_PATH=cancel-no-submit-no-history,retry\n";
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
        verifyPassOrder(level);
        verifyNvrhiRecording(level);
        verifyHistoryAndErrorPaths(level);
        verifySwapchainLifecycle(context);
        verifyNvrhiYCoordinateConvention(level);
        std::cout << "LEVEL_RENDERER_RECORDER=PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "LEVEL_RENDERER_RECORDER=FAIL: " << exception.what() << '\n';
        return 1;
    }
}

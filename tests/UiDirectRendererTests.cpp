#include "render/presentation/UiRenderer.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    std::string readText(const char* relativePath) {
        std::ifstream input(std::string(LUMIN_TEST_SOURCE_DIR) + "/" + relativePath);
        require(input.is_open(), "Required UI source file must be readable.");
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

    void testBufferGrowth() {
        using lumin::render::UiRenderer;
        require(UiRenderer::growBufferCapacity(0, 1'000, 5'000) == 5'000,
                "Initial vertex buffer capacity must honor the minimum allocation.");
        require(UiRenderer::growBufferCapacity(5'000, 5'001, 5'000) >= 7'500, "A full buffer must grow geometrically.");
        require(UiRenderer::growBufferCapacity(8'000, 7'999, 5'000) == 8'000,
                "A sufficiently large buffer must be reused.");
    }

    void testNvrhiProjectionMapsUiTopToPositiveNdcY() {
        constexpr float displayWidth = 1280.0f;
        constexpr float displayHeight = 720.0f;
        const lumin::render::UiProjection projection =
            lumin::render::UiRenderer::makeNvrhiProjection(0.0f, 0.0f, displayWidth, displayHeight);
        const auto projectY = [&projection](float y) {
            return y * projection.scaleY + projection.translateY;
        };
        require(std::fabs(projectY(0.0f) - 1.0f) < 0.0001f,
                "NvRHI logical viewport requires UI top to map to positive NDC Y.");
        require(std::fabs(projectY(displayHeight) + 1.0f) < 0.0001f,
                "NvRHI logical viewport requires UI bottom to map to negative NDC Y.");
    }

    void testDirectDrawDataContract() {
        const std::string frontend = readText("render/editor/ImGuiFrontend.cpp");
        const std::string renderer = readText("render/presentation/UiRenderer.cpp");
        const std::string runtime = readText("render/runtime/Renderer.cpp");

        require(frontend.find("ImGui_ImplSDL3_NewFrame") != std::string::npos &&
                    frontend.find("return ImGui::GetDrawData()") != std::string::npos &&
                    frontend.find("SetTexID") == std::string::npos,
                "The OS main thread frontend must return current-frame ImDrawData directly.");
        const std::size_t buildFontAtlas = renderer.find("atlas.GetTexDataAsRGBA32");
        const std::size_t publishFontTexture = renderer.find("atlas.SetTexID");
        require(buildFontAtlas != std::string::npos && publishFontTexture > buildFontAtlas,
                "The ImGui font texture id must only be published after the 1.92 legacy atlas is built.");
        require(renderer.find("for (const ImDrawList* list : drawData.CmdLists)") != std::string::npos &&
                    renderer.find("draw.UserCallback(list, &draw)") != std::string::npos &&
                    renderer.find("ImDrawCallback_ResetRenderState") != std::string::npos,
                "The synchronous UI renderer must traverse draw lists and preserve Dear ImGui callbacks.");
        require(renderer.find("sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT") !=
                        std::string::npos &&
                    renderer.find("device_->mapBuffer") != std::string::npos &&
                    renderer.find("commandList.drawIndexed(arguments)") != std::string::npos,
                "Direct UI rendering must support the configured index width and frame-slot uploads.");
        require(runtime.find("std::thread") == std::string::npos && runtime.find("RenderMailbox") == std::string::npos,
                "Renderer must execute synchronously on the SDL/Vulkan owning main thread.");
    }

} // namespace

int main() {
    try {
        testBufferGrowth();
        testNvrhiProjectionMapsUiTopToPositiveNdcY();
        testDirectDrawDataContract();
        std::cout << "UiDirectRenderer PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "UiDirectRenderer FAIL: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}

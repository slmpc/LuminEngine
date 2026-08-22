#include "render/editor/ImGuiFrontend.hpp"
#include "render/presentation/UiRenderer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

namespace {

    using lumin::render::ImGuiFrontend;
    using lumin::render::UiRenderer;
    using lumin::render::core::UiDrawCommandType;

    void require(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(EXIT_FAILURE);
        }
    }

    template <typename Exception, typename Function> void requireThrows(Function&& function, const char* message) {
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            return;
        }
        require(false, message);
    }

    struct SyntheticDrawData {
        ImDrawData drawData;
        std::vector<ImDrawList*> lists;

        SyntheticDrawData(ImVec2 displayPos, ImVec2 displaySize, ImVec2 framebufferScale) : drawData(), lists() {
            drawData.Valid = true;
            drawData.DisplayPos = displayPos;
            drawData.DisplaySize = displaySize;
            drawData.FramebufferScale = framebufferScale;
        }

        ~SyntheticDrawData() {
            for (ImDrawList* list : lists) {
                IM_DELETE(list);
            }
        }

        ImDrawList& addList(int vertexCount, int indexCount) {
            ImDrawList* list = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
            list->VtxBuffer.resize(vertexCount);
            list->IdxBuffer.resize(indexCount);
            lists.push_back(list);
            drawData.CmdLists.push_back(list);
            drawData.CmdListsCount = drawData.CmdLists.Size;
            drawData.TotalVtxCount += vertexCount;
            drawData.TotalIdxCount += indexCount;
            return *list;
        }
    };

    void userCallback(const ImDrawList*, const ImDrawCmd*) {
    }

    std::string readText(const char* relativePath) {
        std::ifstream input(std::string(LUMIN_TEST_SOURCE_DIR) + "/" + relativePath);
        require(input.is_open(), "Required UI source file must be readable.");
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

    std::size_t countOccurrences(const std::string& text, const std::string& needle) {
        std::size_t count = 0;
        std::size_t position = 0;
        while ((position = text.find(needle, position)) != std::string::npos) {
            ++count;
            position += needle.size();
        }
        return count;
    }

    void testBufferGrowth() {
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
            UiRenderer::makeNvrhiProjection(0.0f, 0.0f, displayWidth, displayHeight);
        const auto projectY = [&projection](float y) {
            return y * projection.scaleY + projection.translateY;
        };
        require(std::fabs(projectY(0.0f) - 1.0f) < 0.0001f,
                "NvRHI logical viewport requires UI top to map to positive NDC Y.");
        require(std::fabs(projectY(displayHeight) + 1.0f) < 0.0001f,
                "NvRHI logical viewport requires UI bottom to map to negative NDC Y.");
    }

    void testPacketDeepCopiesGeometryAndCommands() {
        constexpr std::uintptr_t textureId = 0x1234U;
        SyntheticDrawData synthetic({10.0f, 20.0f}, {100.0f, 50.0f}, {2.0f, 2.0f});
        ImDrawList& first = synthetic.addList(20, 30);
        first.VtxBuffer[0].pos = {7.0f, 9.0f};
        first.VtxBuffer[0].uv = {0.25f, 0.75f};
        first.VtxBuffer[0].col = 0xAABBCCDDU;
        first.IdxBuffer[0] = static_cast<ImDrawIdx>(11);
        ImDrawCmd clipped;
        clipped.ClipRect = {5.0f, 15.0f, 40.25f, 45.25f};
        clipped.ElemCount = 6;
        clipped.IdxOffset = 3;
        clipped.VtxOffset = 4;
        clipped.TexRef = ImTextureRef(static_cast<ImTextureID>(textureId));
        first.CmdBuffer.push_back(clipped);

        ImDrawCmd fullyClipped;
        fullyClipped.ClipRect = {-100.0f, -100.0f, 0.0f, 0.0f};
        fullyClipped.ElemCount = 3;
        first.CmdBuffer.push_back(fullyClipped);
        ImDrawCmd malformed;
        malformed.ClipRect = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f};
        malformed.ElemCount = 3;
        first.CmdBuffer.push_back(malformed);

        ImDrawList& second = synthetic.addList(10, 12);
        ImDrawCmd reset;
        reset.UserCallback = ImDrawCallback_ResetRenderState;
        second.CmdBuffer.push_back(reset);
        ImDrawCmd draw;
        draw.ClipRect = {15.0f, 25.0f, 80.0f, 60.0f};
        draw.ElemCount = 9;
        draw.IdxOffset = 2;
        draw.VtxOffset = 1;
        second.CmdBuffer.push_back(draw);

        const lumin::render::core::UiDrawPacket packet = ImGuiFrontend::buildDrawPacket(synthetic.drawData);
        require(packet.vertices.size() == 30 && packet.indices.size() == 42,
                "UiDrawPacket must own all merged draw-list geometry.");
        require(packet.vertices[0].positionX == 7.0f && packet.vertices[0].textureV == 0.75f &&
                    packet.vertices[0].color == 0xAABBCCDDU && packet.indices[0] == 11,
                "UiDrawPacket must preserve vertex and index values.");
        require(packet.commands.size() == 3, "Clipped and malformed commands must not emit packet commands.");
        require(packet.commands[0].type == UiDrawCommandType::Draw && packet.commands[0].scissorLeft == 0 &&
                    packet.commands[0].scissorTop == 0 && packet.commands[0].scissorRight == 61 &&
                    packet.commands[0].scissorBottom == 51,
                "Clip rectangles must be transformed, clamped and conservatively rounded.");
        require(packet.commands[0].indexOffset == 3 && packet.commands[0].vertexOffset == 4 &&
                    packet.commands[0].texture.value() == textureId,
                "Packet commands must preserve offsets and stable logical texture IDs.");
        require(packet.commands[1].type == UiDrawCommandType::ResetRenderState,
                "Reset callbacks must become pointer-free reset-state commands.");
        require(packet.commands[2].indexOffset == 32 && packet.commands[2].vertexOffset == 21 &&
                    packet.commands[2].scissorRight == 140 && packet.commands[2].scissorBottom == 80,
                "Packet offsets must include prior lists and clamp to framebuffer bounds.");

        first.VtxBuffer[0].pos = {-999.0f, -999.0f};
        first.IdxBuffer[0] = 0;
        first.CmdBuffer[0].ElemCount = 0;
        require(packet.vertices[0].positionX == 7.0f && packet.indices[0] == 11 && packet.commands[0].elementCount == 6,
                "Packet data must not alias source ImGui buffers or commands.");
    }

    void testArbitraryCallbacksAreRejected() {
        SyntheticDrawData synthetic({0.0f, 0.0f}, {100.0f, 50.0f}, {1.0f, 1.0f});
        ImDrawList& list = synthetic.addList(3, 3);
        ImDrawCmd callback;
        callback.UserCallback = userCallback;
        list.CmdBuffer.push_back(callback);
        requireThrows<std::invalid_argument>(
            [&] {
                static_cast<void>(ImGuiFrontend::buildDrawPacket(synthetic.drawData));
            },
            "Arbitrary ImGui callbacks must never cross into the render thread.");
    }

    void testEmptyAndInvalidDrawData() {
        ImDrawData empty;
        empty.Valid = true;
        empty.DisplaySize = {1280.0f, 720.0f};
        empty.FramebufferScale = {1.0f, 1.0f};
        require(!ImGuiFrontend::buildDrawPacket(empty).isRenderable(), "Empty draw data must emit no GPU geometry.");

        SyntheticDrawData zeroFramebuffer({0.0f, 0.0f}, {0.0f, 720.0f}, {1.0f, 1.0f});
        zeroFramebuffer.addList(3, 3);
        require(!ImGuiFrontend::buildDrawPacket(zeroFramebuffer.drawData).isRenderable(),
                "Zero-sized framebuffers must emit no GPU geometry.");
    }

    void testFrontendAndRendererSeparationContract() {
        const std::string frontend = readText("render/editor/ImGuiFrontend.cpp");
        const std::string renderer = readText("render/presentation/UiRenderer.cpp");
        const std::string presentation = readText("render/presentation/PresentationRenderer.cpp");

        require(frontend.find("ImGui_ImplSDL3_NewFrame") != std::string::npos &&
                    frontend.find("ImGui::Render()") != std::string::npos,
                "Only the main-thread frontend must own SDL and ImGui frame construction.");
        require(renderer.find("ImGui::") == std::string::npos && renderer.find("ImGui_Impl") == std::string::npos &&
                    renderer.find("UserCallback") == std::string::npos,
                "The render-thread UI renderer must not access ImGui state or callbacks.");
        require(frontend.find("forbids arbitrary Dear ImGui user callbacks") != std::string::npos,
                "Packet capture must explicitly reject arbitrary callbacks.");
        require(countOccurrences(renderer, ".setCpuAccess(nvrhi::CpuAccessMode::Write)") == 2 &&
                    renderer.find("device_->mapBuffer") != std::string::npos &&
                    renderer.find("device_->unmapBuffer") != std::string::npos,
                "Fence-safe per-slot UI geometry must use CPU-writable mapped buffers.");
        require(renderer.find("textureBindings_.find") != std::string::npos &&
                    presentation.find("renderer_.registerTexture(viewportTextureId(), texture)") != std::string::npos,
                "Presentation must resolve stable logical IDs instead of casting GPU binding pointers.");
        require(renderer.find("reinterpret_cast<nvrhi::IBindingSet*>") == std::string::npos &&
                    renderer.find("vkCmd") == std::string::npos,
                "The UI renderer must not decode native pointers or issue raw Vulkan commands.");
        require(renderer.find("outputIsSrgb_ ? 1.0f : 0.0f") != std::string::npos,
                "Presentation must preserve the swapchain transfer-function shader option.");

        const std::string shader = readText("shaders/ImGui.slang");
        require(shader.find("constants.outputConfig.x > 0.5") != std::string::npos &&
                    shader.find("vertexColor.rgb <= 0.04045") != std::string::npos,
                "UI vertex colors must be linearized exactly once for an sRGB attachment.");
    }

} // namespace

int main() {
    ImGui::CreateContext();
    testBufferGrowth();
    testNvrhiProjectionMapsUiTopToPositiveNdcY();
    testPacketDeepCopiesGeometryAndCommands();
    testArbitraryCallbacksAreRejected();
    testEmptyAndInvalidDrawData();
    testFrontendAndRendererSeparationContract();
    ImGui::DestroyContext();
    std::puts("PASS: immutable UiDrawPacket and split UI frontend/renderer contracts are stable.");
    return EXIT_SUCCESS;
}

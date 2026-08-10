#include "render/ImGuiLayer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

namespace {

    using lumin::render::ImGuiDrawEvent;
    using lumin::render::ImGuiLayer;

    void require(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(EXIT_FAILURE);
        }
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
        require(input.is_open(), "Required ImGui renderer source file must be readable.");
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
        require(ImGuiLayer::growBufferCapacity(0, 1'000, 5'000) == 5'000,
                "Initial vertex buffer capacity must honor the minimum allocation.");
        require(ImGuiLayer::growBufferCapacity(5'000, 5'001, 5'000) >= 7'500, "A full buffer must grow geometrically.");
        require(ImGuiLayer::growBufferCapacity(8'000, 7'999, 5'000) == 8'000,
                "A sufficiently large buffer must be reused.");
    }

    void testNvrhiProjectionMapsImGuiTopToPositiveNdcY() {
        constexpr float displayWidth = 1280.0f;
        constexpr float displayHeight = 720.0f;

        const lumin::render::ImGuiProjection projection =
            ImGuiLayer::makeNvrhiProjection(0.0f, 0.0f, displayWidth, displayHeight);
        const auto projectY = [&projection](float y) {
            return y * projection.scaleY + projection.translateY;
        };

        require(std::fabs(projectY(0.0f) - 1.0f) < 0.0001f,
                "NvRHI logical viewport requires ImGui top to map to positive NDC Y.");
        require(std::fabs(projectY(displayHeight) + 1.0f) < 0.0001f,
                "NvRHI logical viewport requires ImGui bottom to map to negative NDC Y.");
    }

    void testScissorsOffsetsAndCallbacks() {
        SyntheticDrawData synthetic({10.0f, 20.0f}, {100.0f, 50.0f}, {2.0f, 2.0f});
        ImDrawList& first = synthetic.addList(20, 30);
        ImDrawCmd clipped;
        clipped.ClipRect = {5.0f, 15.0f, 40.25f, 45.25f};
        clipped.ElemCount = 6;
        clipped.IdxOffset = 3;
        clipped.VtxOffset = 4;
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
        ImDrawCmd callback;
        callback.UserCallback = userCallback;
        second.CmdBuffer.push_back(callback);
        ImDrawCmd draw;
        draw.ClipRect = {15.0f, 25.0f, 80.0f, 60.0f};
        draw.ElemCount = 9;
        draw.IdxOffset = 2;
        draw.VtxOffset = 1;
        second.CmdBuffer.push_back(draw);

        const std::vector<ImGuiDrawEvent> events = ImGuiLayer::buildDrawEvents(synthetic.drawData);
        require(events.size() == 4, "Fully clipped and malformed commands must not emit draw events.");
        require(events[0].type == ImGuiDrawEvent::Type::Draw, "The first command must be a draw.");
        require(events[0].scissorLeft == 0 && events[0].scissorTop == 0 && events[0].scissorRight == 61 &&
                    events[0].scissorBottom == 51,
                "Clip rectangles must be transformed, clamped and conservatively rounded.");
        require(events[0].indexOffset == 3 && events[0].vertexOffset == 4,
                "First-list local offsets must be preserved.");
        require(events[1].type == ImGuiDrawEvent::Type::ResetRenderState,
                "Reset callbacks must become reset-state events.");
        require(events[2].type == ImGuiDrawEvent::Type::UserCallback && events[2].command == &second.CmdBuffer[1],
                "User callbacks must remain ordered and retain their source command.");
        require(events[3].indexOffset == 32 && events[3].vertexOffset == 21,
                "Draw offsets must include preceding command-list buffers.");
        require(events[3].scissorRight == 140 && events[3].scissorBottom == 80,
                "Clip rectangles must be clamped to framebuffer bounds.");
    }

    void testEmptyAndInvalidDrawData() {
        ImDrawData empty;
        empty.Valid = true;
        empty.DisplaySize = {1280.0f, 720.0f};
        empty.FramebufferScale = {1.0f, 1.0f};
        require(ImGuiLayer::buildDrawEvents(empty).empty(), "Empty draw data must emit no GPU work.");

        SyntheticDrawData zeroFramebuffer({0.0f, 0.0f}, {0.0f, 720.0f}, {1.0f, 1.0f});
        zeroFramebuffer.addList(3, 3);
        require(ImGuiLayer::buildDrawEvents(zeroFramebuffer.drawData).empty(),
                "Zero-sized framebuffers must emit no GPU work.");
    }

    void testBackendIntegrationContract() {
        const std::string layer = readText("render/ImGuiLayer.cpp");
        const std::string manager = readText("render/ImGuiManager.cpp");
        require(countOccurrences(layer, "createFontResources();") == 1,
                "Font texture and binding initialization must run exactly once per layer initialization.");
        require(layer.find("device_->createTexture(textureDesc)") != std::string::npos &&
                    layer.find("device_->createBindingSet(bindingSetDesc, bindingLayout_)") != std::string::npos,
                "The custom renderer must create its font texture and binding set through NvRHI.");
        require(layer.find("ImGui_ImplSDL3_NewFrame") != std::string::npos,
                "The SDL3 platform new-frame backend must remain active.");
        require(
            countOccurrences(layer, ".setCpuAccess(nvrhi::CpuAccessMode::Write)") == 2 &&
                layer.find(".setIsVolatile(true)") == std::string::npos,
            "NvRHI Vulkan only permits volatile constant buffers; ImGui vertex/index buffers must be CPU-writable.");
        require(layer.find("device_->mapBuffer") != std::string::npos &&
                    layer.find("device_->unmapBuffer") != std::string::npos &&
                    layer.find("commandList.writeBuffer") == std::string::npos,
                "Per-frame ImGui geometry must use fence-safe mapped writes without runtime upload barriers.");
        require(layer.find("ImGui_ImplVulkan") == std::string::npos && layer.find("vkCmd") == std::string::npos &&
                    manager.find("vkCmd") == std::string::npos,
                "The custom renderer must not retain the Vulkan ImGui backend or raw Vulkan draw commands.");
        require(manager.find("if (!layer_.initialized() || !framePrepared_") != std::string::npos &&
                    manager.find("ImGui::EndFrame();") != std::string::npos,
                "Unprepared record and cancel paths must preserve the established frame-state guards.");
    }

} // namespace

int main() {
    ImGui::CreateContext();
    testBufferGrowth();
    testNvrhiProjectionMapsImGuiTopToPositiveNdcY();
    testScissorsOffsetsAndCallbacks();
    testEmptyAndInvalidDrawData();
    testBackendIntegrationContract();
    ImGui::DestroyContext();
    std::puts("PASS: synthetic ImDrawData produces stable NvRHI draw events and safe empty paths.");
    return EXIT_SUCCESS;
}

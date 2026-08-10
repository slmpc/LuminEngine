#include "EditorStyle.hpp"

#include <imgui.h>

namespace lumin::editor::style {
    namespace {

        ImVec4 color(unsigned int rgba) {
            constexpr float scale = 1.0f / 255.0f;
            return {static_cast<float>((rgba >> 24U) & 0xffU) * scale,
                    static_cast<float>((rgba >> 16U) & 0xffU) * scale, static_cast<float>((rgba >> 8U) & 0xffU) * scale,
                    static_cast<float>(rgba & 0xffU) * scale};
        }

    } // namespace

    void apply() {
        ImGuiStyle& value = ImGui::GetStyle();
        value.WindowPadding = {Space2, Space2};
        value.FramePadding = {Space2, Space1};
        value.ItemSpacing = {Space2, Space1};
        value.ItemInnerSpacing = {Space1, Space1};
        value.IndentSpacing = Space4;
        value.ScrollbarSize = Space3;
        value.GrabMinSize = Space2;
        value.WindowRounding = Space0;
        value.ChildRounding = Space0;
        value.FrameRounding = 2.0f;
        value.PopupRounding = 2.0f;
        value.ScrollbarRounding = 2.0f;
        value.GrabRounding = 2.0f;
        value.TabRounding = 2.0f;
        value.WindowBorderSize = 1.0f;
        value.ChildBorderSize = Space0;
        value.PopupBorderSize = 1.0f;
        value.FrameBorderSize = Space0;

        ImVec4* colors = value.Colors;
        colors[ImGuiCol_Text] = color(0xD7D9DAFF);
        colors[ImGuiCol_TextDisabled] = color(0x767B7EFF);
        colors[ImGuiCol_WindowBg] = color(0x191B1DFF);
        colors[ImGuiCol_ChildBg] = color(0x191B1DFF);
        colors[ImGuiCol_PopupBg] = color(0x202326FF);
        colors[ImGuiCol_Border] = color(0x34383CFF);
        colors[ImGuiCol_BorderShadow] = color(0x0D0F1000);
        colors[ImGuiCol_FrameBg] = color(0x24272AFF);
        colors[ImGuiCol_FrameBgHovered] = color(0x303438FF);
        colors[ImGuiCol_FrameBgActive] = color(0x3A3F44FF);
        colors[ImGuiCol_TitleBg] = color(0x17191BFF);
        colors[ImGuiCol_TitleBgActive] = color(0x202326FF);
        colors[ImGuiCol_TitleBgCollapsed] = color(0x17191BFF);
        colors[ImGuiCol_MenuBarBg] = color(0x1D2022FF);
        colors[ImGuiCol_ScrollbarBg] = color(0x151719FF);
        colors[ImGuiCol_ScrollbarGrab] = color(0x34383CFF);
        colors[ImGuiCol_ScrollbarGrabHovered] = color(0x444A4FFF);
        colors[ImGuiCol_ScrollbarGrabActive] = color(0x565E64FF);
        colors[ImGuiCol_CheckMark] = color(0xD0A24BFF);
        colors[ImGuiCol_SliderGrab] = color(0xD0A24BFF);
        colors[ImGuiCol_SliderGrabActive] = color(0xE0B65FFF);
        colors[ImGuiCol_Button] = color(0x282C2FFF);
        colors[ImGuiCol_ButtonHovered] = color(0x363B3FFF);
        colors[ImGuiCol_ButtonActive] = color(0x202326FF);
        colors[ImGuiCol_Header] = color(0x2B2F32FF);
        colors[ImGuiCol_HeaderHovered] = color(0x363B3FFF);
        colors[ImGuiCol_HeaderActive] = color(0x24272AFF);
        colors[ImGuiCol_Separator] = color(0x34383CFF);
        colors[ImGuiCol_SeparatorHovered] = color(0x555C61FF);
        colors[ImGuiCol_SeparatorActive] = color(0xD0A24BFF);
        colors[ImGuiCol_ResizeGrip] = color(0x0D0F1000);
        colors[ImGuiCol_ResizeGripHovered] = color(0x555C61FF);
        colors[ImGuiCol_ResizeGripActive] = color(0xD0A24BFF);
        colors[ImGuiCol_Tab] = color(0x1D2022FF);
        colors[ImGuiCol_TabHovered] = color(0x34383CFF);
        colors[ImGuiCol_TabSelected] = color(0x292D30FF);
        colors[ImGuiCol_TabDimmed] = color(0x17191BFF);
        colors[ImGuiCol_TabDimmedSelected] = color(0x24272AFF);
        colors[ImGuiCol_DockingPreview] = color(0xD0A24BA6);
        colors[ImGuiCol_DockingEmptyBg] = color(0x131517FF);
        colors[ImGuiCol_PlotLines] = color(0x91999EFF);
        colors[ImGuiCol_PlotLinesHovered] = color(0xE0B65FFF);
        colors[ImGuiCol_PlotHistogram] = color(0xD0A24BFF);
        colors[ImGuiCol_PlotHistogramHovered] = color(0xE0B65FFF);
        colors[ImGuiCol_TableHeaderBg] = color(0x24272AFF);
        colors[ImGuiCol_TableBorderStrong] = color(0x34383CFF);
        colors[ImGuiCol_TableBorderLight] = color(0x292C2FFF);
        colors[ImGuiCol_TableRowBg] = color(0x00000000);
        colors[ImGuiCol_TableRowBgAlt] = color(0xFFFFFF05);
        colors[ImGuiCol_TextSelectedBg] = color(0x4A566080);
        colors[ImGuiCol_DragDropTarget] = color(0xE0B65FFF);
        colors[ImGuiCol_NavCursor] = color(0xD0A24BFF);
        colors[ImGuiCol_NavWindowingHighlight] = color(0xFFFFFFB3);
        colors[ImGuiCol_NavWindowingDimBg] = color(0x11131533);
        colors[ImGuiCol_ModalWindowDimBg] = color(0x070809B8);
    }

} // namespace lumin::editor::style

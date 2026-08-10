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
        colors[ImGuiCol_Text] = color(0xEDF1F3FF);
        colors[ImGuiCol_TextDisabled] = color(0x69737AFF);
        colors[ImGuiCol_WindowBg] = color(0x171A1DFF);
        colors[ImGuiCol_ChildBg] = color(0x1C2024FF);
        colors[ImGuiCol_PopupBg] = color(0x23282CFF);
        colors[ImGuiCol_Border] = color(0x626D73FF);
        colors[ImGuiCol_BorderShadow] = color(0x11131500);
        colors[ImGuiCol_FrameBg] = color(0x202529FF);
        colors[ImGuiCol_FrameBgHovered] = color(0x2A3136FF);
        colors[ImGuiCol_FrameBgActive] = color(0x343D42FF);
        colors[ImGuiCol_TitleBg] = color(0x171A1DFF);
        colors[ImGuiCol_TitleBgActive] = color(0x1C2024FF);
        colors[ImGuiCol_TitleBgCollapsed] = color(0x171A1DFF);
        colors[ImGuiCol_MenuBarBg] = color(0x171A1DFF);
        colors[ImGuiCol_ScrollbarBg] = color(0x171A1DFF);
        colors[ImGuiCol_ScrollbarGrab] = color(0x626D73FF);
        colors[ImGuiCol_ScrollbarGrabHovered] = color(0x69737AFF);
        colors[ImGuiCol_ScrollbarGrabActive] = color(0x909BA1FF);
        colors[ImGuiCol_CheckMark] = color(0x50C7A9FF);
        colors[ImGuiCol_SliderGrab] = color(0x50C7A9FF);
        colors[ImGuiCol_SliderGrabActive] = color(0x6BD8BCFF);
        colors[ImGuiCol_Button] = color(0x202529FF);
        colors[ImGuiCol_ButtonHovered] = color(0x2A3136FF);
        colors[ImGuiCol_ButtonActive] = color(0x343D42FF);
        colors[ImGuiCol_Header] = color(0x264740FF);
        colors[ImGuiCol_HeaderHovered] = color(0x2A3136FF);
        colors[ImGuiCol_HeaderActive] = color(0x343D42FF);
        colors[ImGuiCol_Separator] = color(0x2C3237FF);
        colors[ImGuiCol_SeparatorHovered] = color(0x39AA8FFF);
        colors[ImGuiCol_SeparatorActive] = color(0x50C7A9FF);
        colors[ImGuiCol_ResizeGrip] = color(0x11131500);
        colors[ImGuiCol_ResizeGripHovered] = color(0x39AA8FFF);
        colors[ImGuiCol_ResizeGripActive] = color(0x50C7A9FF);
        colors[ImGuiCol_Tab] = color(0x202529FF);
        colors[ImGuiCol_TabHovered] = color(0x2A3136FF);
        colors[ImGuiCol_TabSelected] = color(0x1C2024FF);
        colors[ImGuiCol_TabDimmed] = color(0x202529FF);
        colors[ImGuiCol_TabDimmedSelected] = color(0x343D42FF);
        colors[ImGuiCol_DockingPreview] = color(0x76D0FFFF);
        colors[ImGuiCol_DockingEmptyBg] = color(0x111315FF);
        colors[ImGuiCol_TextSelectedBg] = color(0x264740FF);
        colors[ImGuiCol_DragDropTarget] = color(0xE5B85CFF);
        colors[ImGuiCol_NavCursor] = color(0x76D0FFFF);
        colors[ImGuiCol_ModalWindowDimBg] = color(0x080A0BC7);
    }

} // namespace lumin::editor::style

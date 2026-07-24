#pragma once

namespace lumin::editor::style {

    inline constexpr float Space0 = 0.0f;
    inline constexpr float Space1 = 4.0f;
    inline constexpr float Space2 = 8.0f;
    inline constexpr float Space3 = 12.0f;
    inline constexpr float Space4 = 16.0f;
    inline constexpr float RowHeight = 24.0f;
    inline constexpr float PropertyLabelWidth = 120.0f;
    inline constexpr float HierarchyRatio = 0.1875f;
    inline constexpr float PropertiesRatio = 0.25f;
    inline constexpr float ConsoleRatio = 0.30f;
    inline constexpr float InspectorRatio = 0.60f;
    inline constexpr int LayoutSchema = 2;

    void apply();

} // namespace lumin::editor::style

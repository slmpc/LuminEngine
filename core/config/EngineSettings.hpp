#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lumin::config {

    inline constexpr std::uint32_t EngineSettingsFormatVersion = 1;
    inline constexpr std::size_t MaximumRecentProjects = 10;

    enum class StartupDestination {
        ProjectNavigator,
        LastProject,
    };

    struct EditorWindowVisibility {
        bool viewport = true;
        bool sceneHierarchy = true;
        bool details = true;
        bool contentBrowser = true;
        bool scriptConsole = true;
        bool renderSettings = true;
        /** `Project Settings` dock 面板是否可见。 */
        bool projectSettings = true;

        friend bool operator==(const EditorWindowVisibility&, const EditorWindowVisibility&) = default;
    };

    struct EngineSettings {
        StartupDestination startupDestination = StartupDestination::ProjectNavigator;
        std::optional<std::filesystem::path> lastProject;
        std::vector<std::filesystem::path> recentProjects;
        EditorWindowVisibility windows;

        friend bool operator==(const EngineSettings&, const EngineSettings&) = default;
    };

    struct EngineSettingsLoadResult {
        EngineSettings settings;
        std::string diagnostic;
        bool needsSave = false;
    };

    [[nodiscard]] EngineSettingsLoadResult
    loadEngineSettings(const std::filesystem::path& settingsPath,
                       const std::filesystem::path& legacyRecentProjectsPath = {});
    bool saveEngineSettings(const std::filesystem::path& settingsPath, const EngineSettings& settings,
                            std::string& error);
    void normalizeEngineSettings(EngineSettings& settings);
    void rememberProject(EngineSettings& settings, const std::filesystem::path& projectFile);

} // namespace lumin::config

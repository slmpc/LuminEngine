#include "config/EngineSettings.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lumin::config {
    namespace {
        using Json = nlohmann::json;

        const char* startupDestinationName(StartupDestination destination) noexcept {
            switch (destination) {
            case StartupDestination::ProjectNavigator:
                return "ProjectNavigator";
            case StartupDestination::LastProject:
                return "LastProject";
            }
            return "ProjectNavigator";
        }

        StartupDestination parseStartupDestination(std::string_view value) {
            if (value == "ProjectNavigator") {
                return StartupDestination::ProjectNavigator;
            }
            if (value == "LastProject") {
                return StartupDestination::LastProject;
            }
            throw std::runtime_error("Unknown startup destination.");
        }

        std::filesystem::path normalizedAbsolutePath(const std::filesystem::path& path) {
            if (path.empty()) {
                return {};
            }
            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(path, error);
            return (error ? path : absolute).lexically_normal();
        }

        Json settingsJson(const EngineSettings& settings) {
            Json recent = Json::array();
            for (const auto& path : settings.recentProjects) {
                recent.push_back(path.generic_string());
            }
            const EditorWindowVisibility& windows = settings.windows;
            return {{"formatVersion", EngineSettingsFormatVersion},
                    {"startupDestination", startupDestinationName(settings.startupDestination)},
                    {"lastProject", settings.lastProject.has_value() ? settings.lastProject->generic_string() : ""},
                    {"recentProjects", std::move(recent)},
                    {"windows",
                     {{"viewport", windows.viewport},
                      {"sceneHierarchy", windows.sceneHierarchy},
                      {"details", windows.details},
                      {"contentBrowser", windows.contentBrowser},
                      {"scriptConsole", windows.scriptConsole},
                      {"renderSettings", windows.renderSettings},
                      {"projectSettings", windows.projectSettings}}}};
        }

        EngineSettings parseSettings(const Json& document) {
            if (!document.is_object() || document.value("formatVersion", 0U) != EngineSettingsFormatVersion) {
                throw std::runtime_error("Unsupported engine settings format.");
            }
            EngineSettings settings;
            settings.startupDestination =
                parseStartupDestination(document.value("startupDestination", std::string{"ProjectNavigator"}));
            const std::string lastProject = document.value("lastProject", std::string{});
            if (!lastProject.empty()) {
                settings.lastProject = lastProject;
            }
            if (const auto recent = document.find("recentProjects"); recent != document.end()) {
                if (!recent->is_array()) {
                    throw std::runtime_error("Engine settings recentProjects must be an array.");
                }
                for (const auto& path : *recent) {
                    if (!path.is_string()) {
                        throw std::runtime_error("Engine settings project paths must be strings.");
                    }
                    settings.recentProjects.emplace_back(path.get<std::string>());
                }
            }
            if (const auto windows = document.find("windows"); windows != document.end()) {
                if (!windows->is_object()) {
                    throw std::runtime_error("Engine settings windows must be an object.");
                }
                settings.windows.viewport = windows->value("viewport", true);
                settings.windows.sceneHierarchy = windows->value("sceneHierarchy", true);
                settings.windows.details = windows->value("details", true);
                settings.windows.contentBrowser = windows->value("contentBrowser", true);
                settings.windows.scriptConsole = windows->value("scriptConsole", true);
                settings.windows.renderSettings = windows->value("renderSettings", true);
                settings.windows.projectSettings = windows->value("projectSettings", true);
            }
            normalizeEngineSettings(settings);
            return settings;
        }

        std::vector<std::filesystem::path> loadLegacyRecentProjects(const std::filesystem::path& path) {
            std::vector<std::filesystem::path> projects;
            std::ifstream stream(path);
            std::string line;
            while (std::getline(stream, line) && projects.size() < MaximumRecentProjects) {
                if (!line.empty() && std::filesystem::exists(line)) {
                    projects.emplace_back(line);
                }
            }
            return projects;
        }

    } // namespace

    void normalizeEngineSettings(EngineSettings& settings) {
        if (settings.lastProject.has_value()) {
            const std::filesystem::path normalized = normalizedAbsolutePath(*settings.lastProject);
            if (normalized.empty()) {
                settings.lastProject.reset();
            } else {
                settings.lastProject = normalized;
            }
        }

        std::vector<std::filesystem::path> normalized;
        normalized.reserve(std::min(settings.recentProjects.size(), MaximumRecentProjects));
        for (const auto& path : settings.recentProjects) {
            const std::filesystem::path candidate = normalizedAbsolutePath(path);
            if (candidate.empty() || std::ranges::find(normalized, candidate) != normalized.end()) {
                continue;
            }
            normalized.push_back(candidate);
            if (normalized.size() == MaximumRecentProjects) {
                break;
            }
        }
        settings.recentProjects = std::move(normalized);
    }

    void rememberProject(EngineSettings& settings, const std::filesystem::path& projectFile) {
        const std::filesystem::path normalized = normalizedAbsolutePath(projectFile);
        if (normalized.empty()) {
            return;
        }
        settings.lastProject = normalized;
        std::erase(settings.recentProjects, normalized);
        settings.recentProjects.insert(settings.recentProjects.begin(), normalized);
        if (settings.recentProjects.size() > MaximumRecentProjects) {
            settings.recentProjects.resize(MaximumRecentProjects);
        }
    }

    EngineSettingsLoadResult loadEngineSettings(const std::filesystem::path& settingsPath,
                                                const std::filesystem::path& legacyRecentProjectsPath) {
        EngineSettingsLoadResult result;
        if (settingsPath.empty() || !std::filesystem::exists(settingsPath)) {
            if (!legacyRecentProjectsPath.empty() && std::filesystem::exists(legacyRecentProjectsPath)) {
                result.settings.recentProjects = loadLegacyRecentProjects(legacyRecentProjectsPath);
                normalizeEngineSettings(result.settings);
                result.needsSave = !result.settings.recentProjects.empty();
            }
            return result;
        }

        try {
            std::ifstream stream(settingsPath, std::ios::binary);
            if (!stream) {
                throw std::runtime_error("Could not open engine settings.");
            }
            Json document;
            stream >> document;
            result.settings = parseSettings(document);
        } catch (const std::exception& exception) {
            result.settings = {};
            result.diagnostic = "Could not load engine settings from '" + settingsPath.generic_string() +
                                "': " + exception.what() + " Defaults will be used.";
        }
        return result;
    }

    bool saveEngineSettings(const std::filesystem::path& settingsPath, const EngineSettings& settings,
                            std::string& error) {
        if (settingsPath.empty()) {
            error = "Engine settings path is unavailable.";
            return false;
        }
        const std::filesystem::path temporary = settingsPath.string() + ".tmp";
        const std::filesystem::path backup = settingsPath.string() + ".bak";
        try {
            EngineSettings normalized = settings;
            normalizeEngineSettings(normalized);
            std::filesystem::create_directories(settingsPath.parent_path());
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream) {
                    throw std::runtime_error("Could not create temporary settings file.");
                }
                stream << settingsJson(normalized).dump(2) << '\n';
                if (!stream) {
                    throw std::runtime_error("Could not write complete settings data.");
                }
            }
            std::error_code ignored;
            std::filesystem::remove(backup, ignored);
            if (std::filesystem::exists(settingsPath)) {
                std::filesystem::rename(settingsPath, backup);
            }
            try {
                std::filesystem::rename(temporary, settingsPath);
                std::filesystem::remove(backup, ignored);
            } catch (...) {
                if (std::filesystem::exists(backup)) {
                    std::filesystem::rename(backup, settingsPath, ignored);
                }
                throw;
            }
            error.clear();
            return true;
        } catch (const std::exception& exception) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "Failed to save engine settings to '" + settingsPath.generic_string() + "': " + exception.what();
            return false;
        }
    }

} // namespace lumin::config

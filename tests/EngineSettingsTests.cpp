#include "config/EngineSettings.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

    void require(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string{message});
        }
    }

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("lumin-engine-settings-tests-" + std::to_string(suffix));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    void writeText(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
        require(stream.good(), "Test fixture must be written completely.");
    }

    void testDefaultsAndRoundTrip() {
        TemporaryDirectory temporary;
        const auto settingsPath = temporary.path / "engine-settings.json";
        const auto missing = lumin::config::loadEngineSettings(settingsPath);
        require(missing.diagnostic.empty() && !missing.needsSave &&
                    missing.settings.startupDestination == lumin::config::StartupDestination::ProjectNavigator &&
                    missing.settings.windows == lumin::config::EditorWindowVisibility{},
                "Missing settings must use navigator and visible-panel defaults.");

        lumin::config::EngineSettings expected;
        expected.startupDestination = lumin::config::StartupDestination::LastProject;
        expected.windows.viewport = false;
        expected.windows.scriptConsole = false;
        lumin::config::rememberProject(expected, temporary.path / "Example/Example.luminproject");
        std::string error;
        require(lumin::config::saveEngineSettings(settingsPath, expected, error), error);
        require(!std::filesystem::exists(settingsPath.string() + ".tmp") &&
                    !std::filesystem::exists(settingsPath.string() + ".bak"),
                "Successful atomic settings writes must remove temporary files.");
        const auto loaded = lumin::config::loadEngineSettings(settingsPath);
        require(loaded.diagnostic.empty() && loaded.settings == expected,
                "All engine settings must round-trip through JSON.");
    }

    void testInvalidSettingsFallBack() {
        TemporaryDirectory temporary;
        const auto settingsPath = temporary.path / "engine-settings.json";
        writeText(settingsPath, "not json");
        auto loaded = lumin::config::loadEngineSettings(settingsPath);
        require(!loaded.diagnostic.empty() && loaded.settings == lumin::config::EngineSettings{},
                "Malformed settings must report a non-fatal diagnostic and use defaults.");

        writeText(settingsPath, R"({"formatVersion":99})");
        loaded = lumin::config::loadEngineSettings(settingsPath);
        require(!loaded.diagnostic.empty() && loaded.settings == lumin::config::EngineSettings{},
                "Unknown settings versions must use defaults.");
    }

    void testRecentProjectsMigrationAndNormalization() {
        TemporaryDirectory temporary;
        const auto settingsPath = temporary.path / "engine-settings.json";
        const auto legacyPath = temporary.path / "recent-projects.txt";
        std::string legacy;
        for (int index = 0; index < 12; ++index) {
            const auto project = temporary.path / ("Project" + std::to_string(index) + ".luminproject");
            writeText(project, "{}");
            legacy += project.generic_string() + '\n';
        }
        legacy += (temporary.path / "missing.luminproject").generic_string() + '\n';
        writeText(legacyPath, legacy);

        const auto migrated = lumin::config::loadEngineSettings(settingsPath, legacyPath);
        require(migrated.needsSave && migrated.settings.recentProjects.size() == 10,
                "Legacy recents must migrate only existing paths up to the configured limit.");

        lumin::config::EngineSettings settings = migrated.settings;
        const auto existing = settings.recentProjects[5];
        lumin::config::rememberProject(settings, existing);
        require(settings.recentProjects.size() == 10 && settings.recentProjects.front() == existing &&
                    settings.lastProject == existing,
                "Remembering a project must deduplicate it, move it to the front, and update lastProject.");
    }

} // namespace

int main() {
    testDefaultsAndRoundTrip();
    testInvalidSettingsFallBack();
    testRecentProjectsMigrationAndNormalization();
    return 0;
}

#include "project/ProjectSession.hpp"
#include "render/editor/ViewportPicking.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    struct TemporaryDirectory {
        TemporaryDirectory() {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("lumin-project-tests-" + std::to_string(suffix));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }

        std::filesystem::path path;
    };

    void writeText(const std::filesystem::path& path, std::string_view text) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
        if (!stream) {
            throw std::runtime_error("Failed to prepare project test input.");
        }
    }

    void testProjectAssetAndSceneRoundTrip() {
        TemporaryDirectory temporary;

        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.create(temporary.path, "RoundTrip", error), error.c_str());
        require(std::filesystem::exists(project.projectFile()) &&
                    std::filesystem::exists(project.rootDirectory() / "Scenes/Main.lumin.scene"),
                "Project creation must write its manifest and default scene.");

        const auto meshPath = project.rootDirectory() / "Models/triangle.obj";
        const auto scriptPath = project.rootDirectory() / "Gameplay/move.lua";
        std::filesystem::create_directories(meshPath.parent_path());
        std::filesystem::create_directories(scriptPath.parent_path());
        writeText(meshPath, "v -1 0 0\nv 1 0 0\nv 0 2 0\nf 1 2 3\n");
        writeText(scriptPath, "return { on_tick = function(actor, level, dt) end }\n");
        writeText(project.rootDirectory() / "notes.txt", "Visible but not an engine asset.\n");
        const auto discovered = project.synchronizeProjectFiles(true);
        require(discovered.succeeded() && discovered.added == 2,
                "Project files must be discovered without an import operation.");
        const auto* meshAsset = project.assets().findByPath("Models/triangle.obj");
        const auto* scriptAsset = project.assets().findByPath("Gameplay/move.lua");
        require(meshAsset != nullptr && meshAsset->available && scriptAsset != nullptr && scriptAsset->available,
                "Supported files anywhere under the project root must become available assets.");
        const lumin::project::AssetId meshId = meshAsset->id;
        require(std::ranges::any_of(project.projectEntries(),
                                    [](const lumin::project::ProjectEntry& entry) {
                                        return entry.relativePath == "notes.txt" && !entry.asset.has_value();
                                    }),
                "Unsupported project files must remain visible without becoming assets.");

        const auto actorHandle = project.createActorFromMesh(meshId);
        require(actorHandle.has_value(), "A discovered mesh must instantiate a persistent Actor.");
        lumin::scene::Actor* actor = level.actor(*actorHandle);
        actor->setName("Round Trip Actor");
        lumin::scene::Transform transform = actor->transform();
        transform.position = {2.0f, 3.0f, 4.0f};
        actor->setTransform(transform);
        const auto firstScript = scripts.attach(level, *actorHandle, scriptPath);
        const auto secondScript = scripts.attach(level, *actorHandle, scriptPath);
        require(firstScript && secondScript && scripts.setEnabled(secondScript.script, false),
                "Multiple Lua components must attach independently to one Actor.");
        require(scripts.reorder(secondScript.script, 0), "Script component order must be editable.");
        lumin::project::ProjectSettings projectSettings;
        projectSettings.logicTickHz = 144;
        projectSettings.render = {.rayTracing = false,
                                  .ambientOcclusionMode = lumin::project::ProjectAmbientOcclusionMode::Gtao,
                                  .ambientOcclusionRadius = 2.5f,
                                  .ambientOcclusionStrength = 1.4f,
                                  .ambientOcclusionBias = 0.12f};
        project.setSettings(projectSettings);
        project.markDirty();
        require(project.save(error), error.c_str());

        const auto projectFile = project.projectFile();
        const auto scenePath = project.rootDirectory() / "Scenes/Main.lumin.scene";
        nlohmann::json sceneDocument;
        {
            std::ifstream sceneStream(scenePath, std::ios::binary);
            sceneStream >> sceneDocument;
        }
        require(sceneDocument["projectSettings"].value("logicTickHz", 0U) == 144U &&
                    sceneDocument["projectSettings"].contains("render") && !sceneDocument.contains("renderSettings"),
                "Project runtime and render settings must persist under the Project Settings object.");
        sceneDocument["actors"][0]["material"]["textures"] = {
            {"baseColor", ""}, {"normal", ""}, {"roughness", ""}, {"flipNormalY", true}};
        writeText(scenePath, sceneDocument.dump(2));
        require(project.open(projectFile, error), error.c_str());
        require(level.actorCount() == 1, "Scene load must replace the previous Level contents.");
        const lumin::scene::Actor* restored = level.actor(level.actorHandles().front());
        require(restored != nullptr && restored->name() == "Round Trip Actor" &&
                    restored->transform().position == glm::vec3(2.0f, 3.0f, 4.0f),
                "Persistent Actor identity, name, and transform must round-trip.");
        require(!restored->material().textures.has_value(),
                "Legacy empty texture objects must load as an untextured material.");
        const auto restoredScripts = scripts.scriptsForActor(restored->handle());
        require(restoredScripts.size() == 2 && !restoredScripts.front().enabled && restoredScripts.back().enabled,
                "Script order and enabled state must round-trip.");
        const auto& restoredRenderSettings = project.renderSettings();
        require(project.settings().logicTickHz == 144 && !restoredRenderSettings.rayTracing &&
                    restoredRenderSettings.ambientOcclusionMode == lumin::project::ProjectAmbientOcclusionMode::Gtao &&
                    restoredRenderSettings.ambientOcclusionRadius == 2.5f &&
                    restoredRenderSettings.ambientOcclusionStrength == 1.4f &&
                    restoredRenderSettings.ambientOcclusionBias == 0.12f,
                "Project tick rate and render tuning must round-trip through the scene file.");

        require(!project.removeAsset(meshId, error) && !error.empty(),
                "Referenced mesh assets must be protected from deletion.");

        const auto invalidScript = project.rootDirectory() / "invalid.lua";
        writeText(invalidScript, "return { on_tick = function( }\n");
        const auto invalidDiscovery = project.synchronizeProjectFiles(true);
        const auto* invalidAsset = project.assets().findByPath("invalid.lua");
        require(invalidDiscovery.succeeded() && invalidAsset != nullptr && invalidAsset->available,
                "Discovery must create metadata without eagerly validating asset contents.");
        require(!scripts.validate(invalidScript), "Invalid Lua must still fail when it is actually loaded.");

        const auto maliciousProject = temporary.path / "malicious.luminproject";
        writeText(maliciousProject,
                  R"({"formatVersion":1,"name":"Bad","contentDirectory":"Content","defaultScene":"../outside.scene"})");
        error.clear();
        require(!project.open(maliciousProject, error) && level.actorCount() == 1,
                "Path-escaping projects must be rejected before replacing the current scene.");
    }

    void testFilesystemAssetIdentityAndMigration() {
        TemporaryDirectory temporary;
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.create(temporary.path, "Identity", error), error.c_str());

        const std::string meshText = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        const auto original = project.rootDirectory() / "Loose/source.obj";
        std::filesystem::create_directories(original.parent_path());
        writeText(original, meshText);
        require(project.synchronizeProjectFiles(true).added == 1, "Initial project scan must register the mesh.");
        const lumin::project::AssetId originalId = project.assets().findByPath("Loose/source.obj")->id;

        writeText(original, meshText + "# modified\n");
        const auto modified = project.synchronizeProjectFiles(true);
        require(modified.modified == 1 && project.assets().findByPath("Loose/source.obj")->id == originalId,
                "Editing an asset in place must preserve its ID.");

        const auto movedPath = project.rootDirectory() / "Moved/renamed.obj";
        std::filesystem::create_directories(movedPath.parent_path());
        std::filesystem::rename(original, movedPath);
        const auto moved = project.synchronizeProjectFiles(true);
        require(moved.moved == 1 && project.assets().findByPath("Moved/renamed.obj")->id == originalId,
                "A unique fingerprint move must preserve the asset ID.");

        std::filesystem::remove(movedPath);
        const auto missing = project.synchronizeProjectFiles(true);
        require(missing.missing == 1 && !project.assets().find(originalId)->available,
                "Externally deleted assets must remain as unavailable registry records.");
        writeText(movedPath, meshText + "# modified\n");
        static_cast<void>(project.synchronizeProjectFiles(true));
        require(project.assets().findByPath("Moved/renamed.obj")->id == originalId,
                "Restoring a missing asset at its path must recover the original ID.");

        const std::string duplicateText = "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n";
        const auto duplicateA = project.rootDirectory() / "Duplicates/a.obj";
        const auto duplicateB = project.rootDirectory() / "Duplicates/b.obj";
        std::filesystem::create_directories(duplicateA.parent_path());
        writeText(duplicateA, duplicateText);
        writeText(duplicateB, duplicateText);
        static_cast<void>(project.synchronizeProjectFiles(true));
        const lumin::project::AssetId duplicateAId = project.assets().findByPath("Duplicates/a.obj")->id;
        const lumin::project::AssetId duplicateBId = project.assets().findByPath("Duplicates/b.obj")->id;
        std::filesystem::remove(duplicateA);
        std::filesystem::remove(duplicateB);
        static_cast<void>(project.synchronizeProjectFiles(true));
        const auto ambiguousPath = project.rootDirectory() / "Duplicates/ambiguous.obj";
        writeText(ambiguousPath, duplicateText);
        const auto ambiguous = project.synchronizeProjectFiles(true);
        const auto* ambiguousAsset = project.assets().findByPath("Duplicates/ambiguous.obj");
        require(ambiguousAsset != nullptr && ambiguousAsset->id != duplicateAId && ambiguousAsset->id != duplicateBId &&
                    !ambiguous.diagnostics.empty(),
                "Ambiguous fingerprint matches must allocate a new ID and report a diagnostic.");

        require(project.save(error), error.c_str());
        const auto projectFile = project.projectFile();
        const auto registryPath = project.rootDirectory() / ".lumin/AssetRegistry.json";
        nlohmann::json registry;
        {
            std::ifstream stream(registryPath);
            stream >> registry;
        }
        registry["formatVersion"] = 1;
        for (auto& asset : registry["assets"]) {
            asset.erase("fingerprint");
        }
        writeText(registryPath, registry.dump(2));
        project.close();
        require(project.open(projectFile, error), error.c_str());
        require(project.assets().findByPath("Moved/renamed.obj")->id == originalId,
                "Opening a v1 registry must preserve IDs while upgrading fingerprints.");
        {
            std::ifstream stream(registryPath);
            stream >> registry;
        }
        require(registry.value("formatVersion", 0) == 2, "A synchronized legacy registry must be written as v2.");
    }

    lumin::assets::Mesh triangleAtDepth(float z) {
        lumin::assets::Mesh mesh;
        mesh.name = "pick-triangle";
        mesh.vertices = {{{-2.0f, -2.0f, z}, {}, {}}, {{2.0f, -2.0f, z}, {}, {}}, {{0.0f, 2.0f, z}, {}, {}}};
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    void testViewportPickingUsesNearestHit() {
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        camera.setPosition({0.0f, 0.0f, 6.0f});
        camera.setOrientation(-90.0f, 0.0f);
        const auto farMesh = level.addMesh(triangleAtDepth(-2.0f));
        const auto nearMesh = level.addMesh(triangleAtDepth(0.0f));
        const auto farModel = level.addModel(farMesh);
        const auto nearModel = level.addModel(nearMesh);
        static_cast<void>(farModel);

        for (const auto [width, height] : {std::pair{800.0f, 600.0f}, std::pair{1600.0f, 600.0f}}) {
            const auto ray = lumin::editor::makeViewportRay(camera, width * 0.5f, height * 0.5f, width, height);
            const auto picked = lumin::editor::pickViewportModel(level, ray);
            require(picked.has_value() && picked->model == nearModel,
                    "Viewport picking must return the nearest triangle for every aspect ratio.");
        }
        const auto missRay = lumin::editor::makeViewportRay(camera, 0.0f, 0.0f, 800.0f, 600.0f);
        require(!lumin::editor::pickViewportModel(level, missRay).has_value(),
                "Viewport picking must return empty for background clicks.");
    }

    void testNewProjectResetsSceneState() {
        TemporaryDirectory temporary;
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.create(temporary.path, "First", error), error.c_str());

        camera.setPosition({9.0f, 8.0f, 7.0f});
        camera.setOrientation(25.0f, 15.0f);
        lumin::scene::SceneEnvironment changedEnvironment = level.environment();
        changedEnvironment.sun.direction = {1.0f, 0.0f, 0.0f};
        changedEnvironment.sun.illuminanceLux = 42.0f;
        level.setEnvironment(changedEnvironment);
        project.setRenderSettings({.directLighting = false,
                                   .shadows = false,
                                   .rayTracing = false,
                                   .ssao = false,
                                   .sharc = false,
                                   .nrd = false,
                                   .taa = false,
                                   .splitLambda = 0.1f,
                                   .shadowDistance = 5.0f,
                                   .exposure = 3.0f});
        level.spawnActor();

        require(project.create(temporary.path, "Second", error), error.c_str());
        const lumin::scene::Camera defaultCamera;
        const lumin::scene::SceneEnvironment defaultEnvironment;
        const auto& defaults = project.renderSettings();
        require(level.actorCount() == 0 && level.models().empty() && camera.position() == defaultCamera.position() &&
                    camera.yawDegrees() == defaultCamera.yawDegrees() &&
                    camera.pitchDegrees() == defaultCamera.pitchDegrees() && camera.cutEpoch() == 1 &&
                    level.environment().sun.direction == defaultEnvironment.sun.direction &&
                    level.environment().sun.illuminanceLux == defaultEnvironment.sun.illuminanceLux &&
                    defaults.directLighting && defaults.shadows && defaults.rayTracing && defaults.ssao &&
                    defaults.sharc && defaults.nrd && defaults.taa && defaults.exposure == 1.0f &&
                    project.settings().logicTickHz == lumin::project::DefaultLogicTickHz,
                "A new empty project must reset scene, camera, environment, and all Project Settings.");
    }

    void testLegacySsaoSettingsRemainCompatible() {
        TemporaryDirectory temporary;
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.create(temporary.path, "LegacyAo", error), error.c_str());

        const auto projectFile = project.projectFile();
        const auto scenePath = project.rootDirectory() / "Scenes/Main.lumin.scene";
        nlohmann::json scene;
        {
            std::ifstream stream(scenePath, std::ios::binary);
            stream >> scene;
        }
        scene["renderSettings"] = scene["projectSettings"]["render"];
        scene.erase("projectSettings");
        scene["renderSettings"]["ssao"] = false;
        scene["renderSettings"].erase("ambientOcclusionMode");
        scene["renderSettings"].erase("ambientOcclusionRadius");
        scene["renderSettings"].erase("ambientOcclusionStrength");
        scene["renderSettings"].erase("ambientOcclusionBias");
        writeText(scenePath, scene.dump(2));

        require(project.open(projectFile, error), error.c_str());
        const auto& settings = project.renderSettings();
        require(!settings.ssao && settings.ambientOcclusionMode == lumin::project::ProjectAmbientOcclusionMode::Ssao &&
                    settings.ambientOcclusionRadius == 1.0f && settings.ambientOcclusionStrength == 1.0f &&
                    settings.ambientOcclusionBias == 0.08f,
                "Projects with only the legacy ssao flag must load with SSAO defaults.");
        require(project.settings().logicTickHz == lumin::project::DefaultLogicTickHz,
                "Legacy projects without Project Settings must use the default logic tick rate.");
    }

    void testProjectLogicTickRateNormalization() {
        lumin::project::ProjectSettings settings;
        settings.logicTickHz = 1;
        lumin::project::normalizeProjectSettings(settings);
        require(settings.logicTickHz == lumin::project::MinimumLogicTickHz,
                "Project logic tick rates below the supported range must clamp to the minimum.");
        settings.logicTickHz = 1'000;
        lumin::project::normalizeProjectSettings(settings);
        require(settings.logicTickHz == lumin::project::MaximumLogicTickHz,
                "Project logic tick rates above the supported range must clamp to the maximum.");
    }

} // namespace

int main() {
    try {
        testProjectAssetAndSceneRoundTrip();
        testFilesystemAssetIdentityAndMigration();
        testViewportPickingUsesNearestHit();
        testNewProjectResetsSceneState();
        testLegacySsaoSettingsRemainCompatible();
        testProjectLogicTickRateNormalization();
        std::cout << "ProjectEditor PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ProjectEditor FAIL: " << exception.what() << '\n';
        return 1;
    }
}

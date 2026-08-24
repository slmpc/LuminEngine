#include "project/ProjectSession.hpp"
#include "render/editor/ViewportPicking.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

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
                                  .ambientOcclusionBias = 0.12f,
                                  .taaSharpness = 0.8f,
                                  .agx = false,
                                  .bloom = false,
                                  .bloomIntensity = 0.2f,
                                  .bloomThreshold = 2.0f,
                                  .bloomSoftKnee = 0.3f,
                                  .bloomRadius = 2.5f};
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
                    sceneDocument["projectSettings"].contains("render") &&
                    !sceneDocument["projectSettings"]["render"].value("agx", true) &&
                    !sceneDocument["projectSettings"]["render"].value("bloom", true) &&
                    !sceneDocument.contains("renderSettings"),
                "Project runtime and render settings must persist under the Project Settings object.");
        sceneDocument["actors"][0]["material"].erase("ior");
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
        require(std::abs(restored->material().blinnPhong.indexOfRefraction - 1.5f) < 1e-6f,
                "Legacy scenes without IOR must migrate to the standard dielectric value.");
        const auto restoredScripts = scripts.scriptsForActor(restored->handle());
        require(restoredScripts.size() == 2 && !restoredScripts.front().enabled && restoredScripts.back().enabled,
                "Script order and enabled state must round-trip.");
        const auto& restoredRenderSettings = project.renderSettings();
        require(project.settings().logicTickHz == 144 && !restoredRenderSettings.rayTracing &&
                    restoredRenderSettings.ambientOcclusionMode == lumin::project::ProjectAmbientOcclusionMode::Gtao &&
                    restoredRenderSettings.ambientOcclusionRadius == 2.5f &&
                    restoredRenderSettings.ambientOcclusionStrength == 1.4f &&
                    restoredRenderSettings.ambientOcclusionBias == 0.12f &&
                    restoredRenderSettings.taaSharpness == 0.8f && !restoredRenderSettings.agx &&
                    !restoredRenderSettings.bloom && restoredRenderSettings.bloomIntensity == 0.2f &&
                    restoredRenderSettings.bloomThreshold == 2.0f && restoredRenderSettings.bloomSoftKnee == 0.3f &&
                    restoredRenderSettings.bloomRadius == 2.5f,
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

    void testMultiMaterialObjCreatesPersistentActors() {
        TemporaryDirectory temporary;
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.create(temporary.path, "MultiMaterial", error), error.c_str());

        const auto meshDirectory = project.rootDirectory() / "Content/Meshes";
        const auto meshPath = meshDirectory / "multi.obj";
        writeText(meshDirectory / "multi.mtl", R"(newmtl Red
Kd 1 0 0
Ks 0.2 0.2 0.2
Ns 12
Ni 1.5
newmtl Green
Kd 0 1 0
Ks 0.1 0.1 0.1
Ns 64
Ni 2.0
)");
        writeText(meshPath, R"(mtllib multi.mtl
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
usemtl Red
f 1/1/1 2/2/1 3/3/1
usemtl Green
f 1/1/1 3/3/1 4/4/1
)");

        const auto sync = project.synchronizeProjectFiles(true);
        const auto* asset = project.assets().findByPath("Content/Meshes/multi.obj");
        require(sync.succeeded() && asset != nullptr, "Multi-material OBJ must be registered as a Mesh asset.");
        const lumin::project::AssetId assetId = asset->id;
        const auto actors = project.createActorsFromMesh(assetId);
        require(actors.size() == 2 && level.actorCount() == 2,
                "Each distinct OBJ material must create one persistent Actor.");
        const lumin::scene::Actor* red = level.actor(actors[0]);
        const lumin::scene::Actor* green = level.actor(actors[1]);
        require(red != nullptr && green != nullptr && red->modelHandle().isValid() && green->modelHandle().isValid() &&
                    level.model(red->modelHandle()).mesh != level.model(green->modelHandle()).mesh,
                "OBJ material partitions must use independent runtime meshes.");
        require(red->material().surfaceModel == lumin::scene::SurfaceModel::BlinnPhong &&
                    red->material().albedo == glm::vec3(1.0f, 0.0f, 0.0f) &&
                    green->material().albedo == glm::vec3(0.0f, 1.0f, 0.0f) &&
                    std::abs(red->material().blinnPhong.indexOfRefraction - 1.5f) < 1e-6f &&
                    std::abs(green->material().blinnPhong.indexOfRefraction - 2.0f) < 1e-6f,
                "MTL diffuse colors, optical density, and surface model must be imported for every partition.");

        require(project.save(error), error.c_str());
        nlohmann::json sceneDocument;
        {
            std::ifstream stream(project.rootDirectory() / "Scenes/Main.lumin.scene", std::ios::binary);
            stream >> sceneDocument;
        }
        require(sceneDocument["actors"].size() == 2 && sceneDocument["actors"][0].value("meshPart", "") == "Red" &&
                    sceneDocument["actors"][1].value("meshPart", "") == "Green" &&
                    std::abs(sceneDocument["actors"][0]["material"].value("ior", 0.0f) - 1.5f) < 1e-6f &&
                    std::abs(sceneDocument["actors"][1]["material"].value("ior", 0.0f) - 2.0f) < 1e-6f,
                "Scene serialization must persist OBJ material partition names and optical density.");

        const auto projectFile = project.projectFile();
        project.close();
        require(project.open(projectFile, error), error.c_str());
        require(level.actorCount() == 2, "Reopening a project must restore all OBJ material partitions.");
        require(!project.removeAsset(assetId, error) && !error.empty(),
                "Any referenced OBJ partition must protect the source asset from deletion.");
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
                                   .taaSharpness = 0.9f,
                                   .splitLambda = 0.1f,
                                   .shadowDistance = 5.0f,
                                   .exposure = 3.0f,
                                   .agx = false,
                                   .bloom = false,
                                   .bloomIntensity = 0.3f,
                                   .bloomThreshold = 3.0f,
                                   .bloomSoftKnee = 0.2f,
                                   .bloomRadius = 3.0f});
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
                    defaults.sharc && defaults.nrd && defaults.taa && defaults.taaSharpness == 0.5f &&
                    defaults.exposure == 1.0f && defaults.agx && defaults.bloom && defaults.bloomIntensity == 0.08f &&
                    defaults.bloomThreshold == 1.0f && defaults.bloomSoftKnee == 0.5f && defaults.bloomRadius == 1.0f &&
                    project.settings().logicTickHz == lumin::project::DefaultLogicTickHz,
                "A new empty project must reset scene, camera, environment, and all Project Settings.");
    }

    void testLocalLightSceneRoundTripAndNrdSetting() {
        TemporaryDirectory temporary;
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.create(temporary.path, "LocalLights", error), error.c_str());

        lumin::scene::SpotLight invalid;
        invalid.range = 0.0f;
        bool invalidRejected = false;
        try {
            static_cast<void>(project.createLightActor(invalid));
        } catch (const std::invalid_argument&) {
            invalidRejected = true;
        }
        require(invalidRejected && level.actorCount() == 0 && !project.dirty(),
                "Invalid project light creation must fail before spawning or dirtying an Actor.");

        lumin::scene::PointLight point;
        point.color = {1.0f, 0.25f, 0.1f};
        point.luminousIntensityCandela = 1800.0f;
        point.range = 14.0f;
        point.castsShadows = false;
        const auto pointActor = project.createLightActor(point, {.position = {1.0f, 2.0f, 3.0f}});

        lumin::scene::SpotLight spot;
        spot.enabled = false;
        spot.color = {0.1f, 0.4f, 1.0f};
        spot.luminousIntensityCandela = 3200.0f;
        spot.range = 22.0f;
        spot.innerConeAngleDegrees = 15.0f;
        spot.outerConeAngleDegrees = 35.0f;
        const auto spotActor = project.createLightActor(spot, {.rotationDegrees = {5.0f, 45.0f, 0.0f}});
        require(project.dirty() && level.actor(pointActor)->persistentId().size() > 0 &&
                    level.actor(spotActor)->name() == "Spot Light",
                "Project light creation must assign persistent identity, default names and dirty state.");
        require(project.save(error), error.c_str());

        const auto projectFile = project.projectFile();
        const auto scenePath = project.rootDirectory() / "Scenes/Main.lumin.scene";
        nlohmann::json scene;
        {
            std::ifstream stream(scenePath, std::ios::binary);
            stream >> scene;
        }
        require(scene.value("formatVersion", 0U) == 1U && scene["actors"].size() == 2 &&
                    scene["actors"][0]["light"].value("type", "") == "point" &&
                    scene["actors"][1]["light"].value("type", "") == "spot",
                "Point and Spot lights must serialize as optional Actor fields without changing format v1.");
        scene["projectSettings"]["render"]["nrd"] = false;
        writeText(scenePath, scene.dump(2));

        require(project.open(projectFile, error), error.c_str());
        require(level.actorCount() == 2 && !project.settings().render.nrd,
                "Opening a light scene must restore both light Actors and its NRD setting.");
        const auto handles = level.actorHandles();
        const auto& restoredPoint = std::get<lumin::scene::PointLight>(*level.actor(handles[0])->localLight());
        const auto& restoredSpot = std::get<lumin::scene::SpotLight>(*level.actor(handles[1])->localLight());
        require(restoredPoint.color == point.color && restoredPoint.luminousIntensityCandela == 1800.0f &&
                    restoredPoint.range == 14.0f && !restoredPoint.castsShadows && !restoredSpot.enabled &&
                    restoredSpot.color == spot.color && restoredSpot.innerConeAngleDegrees == 15.0f &&
                    restoredSpot.outerConeAngleDegrees == 35.0f,
                "All Point and Spot parameters must round-trip while the NRD setting remains persistent.");
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
                    settings.ambientOcclusionBias == 0.08f && settings.taaSharpness == 0.5f && settings.agx &&
                    settings.bloom && settings.bloomIntensity == 0.08f && settings.bloomThreshold == 1.0f,
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
        settings.render.taaSharpness = 2.0f;
        settings.render.bloomIntensity = -1.0f;
        settings.render.bloomThreshold = -1.0f;
        settings.render.bloomSoftKnee = 2.0f;
        settings.render.bloomRadius = 8.0f;
        lumin::project::normalizeProjectSettings(settings);
        require(settings.logicTickHz == lumin::project::MaximumLogicTickHz && settings.render.taaSharpness == 1.0f &&
                    settings.render.bloomIntensity == 0.0f && settings.render.bloomThreshold == 0.0f &&
                    settings.render.bloomSoftKnee == 1.0f && settings.render.bloomRadius == 4.0f,
                "Project logic, TAA, and Bloom values must clamp to their supported ranges.");
    }

    void inspectProject(const std::filesystem::path& projectFile, std::size_t expectedActorCount) {
        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = projectFile.parent_path()});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.open(projectFile, error), error.c_str());
        require(level.actorCount() == expectedActorCount, "Inspected project has an unexpected Actor count.");
        require(std::ranges::all_of(level.actorHandles(),
                                    [&level](lumin::scene::ActorHandle handle) {
                                        const lumin::scene::Actor* actor = level.actor(handle);
                                        return actor != nullptr && actor->modelHandle().isValid();
                                    }),
                "Every inspected project Actor must own a valid model.");
        require(project.save(error), error.c_str());

        std::size_t triangleCount = 0;
        for (const lumin::assets::Mesh& mesh : level.meshes()) {
            triangleCount += mesh.indices.size() / 3;
        }
        std::cout << "Project inspection PASS: actors=" << level.actorCount() << ", meshes=" << level.meshes().size()
                  << ", triangles=" << triangleCount << ", assets=" << project.assets().assets().size() << '\n';
    }

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view{argv[1]} == "--inspect-project") {
            inspectProject(argv[2], static_cast<std::size_t>(std::stoull(argv[3])));
            return 0;
        }
        testProjectAssetAndSceneRoundTrip();
        testMultiMaterialObjCreatesPersistentActors();
        testFilesystemAssetIdentityAndMigration();
        testViewportPickingUsesNearestHit();
        testNewProjectResetsSceneState();
        testLocalLightSceneRoundTripAndNrdSetting();
        testLegacySsaoSettingsRemainCompatible();
        testProjectLogicTickRateNormalization();
        std::cout << "ProjectEditor PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ProjectEditor FAIL: " << exception.what() << '\n';
        return 1;
    }
}

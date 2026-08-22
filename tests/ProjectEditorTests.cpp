#include "project/ProjectSession.hpp"
#include "render/editor/ViewportPicking.hpp"

#include <chrono>
#include <array>
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
        const auto meshSource = temporary.path / "triangle.obj";
        const auto scriptSource = temporary.path / "move.lua";
        writeText(meshSource, "v -1 0 0\nv 1 0 0\nv 0 2 0\nf 1 2 3\n");
        writeText(scriptSource, "return { on_tick = function(actor, level, dt) end }\n");

        lumin::scene::Level level;
        lumin::scene::Camera camera;
        lumin::scripting::ScriptRuntime scripts({.scriptRoot = temporary.path});
        lumin::project::ProjectSession project(level, camera, scripts);
        std::string error;
        require(project.create(temporary.path, "RoundTrip", error), error.c_str());
        require(std::filesystem::exists(project.projectFile()) &&
                    std::filesystem::exists(project.rootDirectory() / "Scenes/Main.lumin.scene"),
                "Project creation must write its manifest and default scene.");

        const std::array<lumin::project::ImportRequest, 2> requests = {
            lumin::project::ImportRequest{.source = meshSource, .destinationDirectory = {},
                                          .conflict = lumin::project::ImportConflictPolicy::Rename},
            lumin::project::ImportRequest{.source = scriptSource, .destinationDirectory = {},
                                          .conflict = lumin::project::ImportConflictPolicy::Rename},
        };
        const auto imported = project.importAssets(requests);
        if (imported.size() != 2 || !imported[0].succeeded() || !imported[1].succeeded()) {
            throw std::runtime_error("OBJ/Lua import failed: " +
                                     (imported.empty() ? std::string{"missing result"} : imported[0].error) + " | " +
                                     (imported.size() < 2 ? std::string{"missing result"} : imported[1].error));
        }

        const auto actorHandle = project.createActorFromMesh(imported[0].asset->id);
        require(actorHandle.has_value(), "An imported mesh must instantiate a persistent Actor.");
        lumin::scene::Actor* actor = level.actor(*actorHandle);
        actor->setName("Round Trip Actor");
        lumin::scene::Transform transform = actor->transform();
        transform.position = {2.0f, 3.0f, 4.0f};
        actor->setTransform(transform);
        const auto firstScript = scripts.attach(level, *actorHandle,
                                                project.rootDirectory() / imported[1].asset->relativePath);
        const auto secondScript = scripts.attach(level, *actorHandle,
                                                 project.rootDirectory() / imported[1].asset->relativePath);
        require(firstScript && secondScript && scripts.setEnabled(secondScript.script, false),
                "Multiple Lua components must attach independently to one Actor.");
        require(scripts.reorder(secondScript.script, 0), "Script component order must be editable.");
        project.markDirty();
        require(project.save(error), error.c_str());

        const auto projectFile = project.projectFile();
        const auto scenePath = project.rootDirectory() / "Scenes/Main.lumin.scene";
        nlohmann::json sceneDocument;
        {
            std::ifstream sceneStream(scenePath, std::ios::binary);
            sceneStream >> sceneDocument;
        }
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

        require(!project.removeAsset(imported[0].asset->id, error) && !error.empty(),
                "Referenced mesh assets must be protected from deletion.");

        const auto invalidScript = temporary.path / "invalid.lua";
        writeText(invalidScript, "return { on_tick = function( }\n");
        const std::array<lumin::project::ImportRequest, 1> invalidRequests = {
            lumin::project::ImportRequest{.source = invalidScript, .destinationDirectory = {},
                                          .conflict = lumin::project::ImportConflictPolicy::Rename},
        };
        const std::size_t assetCount = project.assets().assets().size();
        const auto invalidImport = project.importAssets(invalidRequests);
        require(invalidImport.size() == 1 && !invalidImport.front().succeeded() &&
                    project.assets().assets().size() == assetCount,
                "Invalid Lua imports must roll back without changing the registry.");

        const auto maliciousProject = temporary.path / "malicious.luminproject";
        writeText(maliciousProject,
                  R"({"formatVersion":1,"name":"Bad","contentDirectory":"Content","defaultScene":"../outside.scene"})");
        error.clear();
        require(!project.open(maliciousProject, error) && level.actorCount() == 1,
                "Path-escaping projects must be rejected before replacing the current scene.");
    }

    lumin::assets::Mesh triangleAtDepth(float z) {
        lumin::assets::Mesh mesh;
        mesh.name = "pick-triangle";
        mesh.vertices = {{{-2.0f, -2.0f, z}, {}, {}},
                         {{2.0f, -2.0f, z}, {}, {}},
                         {{0.0f, 2.0f, z}, {}, {}}};
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

} // namespace

int main() {
    try {
        testProjectAssetAndSceneRoundTrip();
        testViewportPickingUsesNearestHit();
        std::cout << "ProjectEditor PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ProjectEditor FAIL: " << exception.what() << '\n';
        return 1;
    }
}

#include "lumin/assets/ImageLoader.hpp"
#include "lumin/render/ModelRenderer.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/CameraController.hpp"
#include "lumin/scene/Level.hpp"
#include "lumin/scene/Terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include <glm/geometric.hpp>

namespace {

    static_assert(!std::is_copy_constructible_v<lumin::scene::Actor>);
    static_assert(!std::is_copy_assignable_v<lumin::scene::Actor>);
    static_assert(!std::is_move_constructible_v<lumin::scene::Actor>);
    static_assert(!std::is_move_assignable_v<lumin::scene::Actor>);

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool nearlyEqual(float left, float right) {
        return std::abs(left - right) < 0.0001f;
    }

    lumin::assets::Mesh makeTriangle(const char* name) {
        lumin::assets::Mesh mesh;
        mesh.name = name;
        mesh.vertices.resize(3);
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    lumin::assets::Mesh makeQuad(const char* name) {
        lumin::assets::Mesh mesh;
        mesh.name = name;
        mesh.vertices.resize(4);
        mesh.indices = {0, 1, 2, 2, 3, 0};
        return mesh;
    }

    void testCameraMovement() {
        lumin::scene::Camera camera;
        camera.setPosition({0.0f, 0.0f, 0.0f});
        camera.setMoveSpeed(4.0f);

        lumin::scene::CameraController::update(camera, {.forward = 1.0f}, 0.5f);
        require(nearlyEqual(camera.position().z, -2.0f), "W must move the camera forward using delta time.");

        lumin::scene::CameraController::update(camera, {.right = 1.0f}, 0.25f);
        require(nearlyEqual(camera.position().x, 1.0f), "D must move the camera along its local right axis.");

        lumin::scene::CameraController::update(camera, {.forward = -1.0f, .right = -1.0f}, 0.25f);
        require(nearlyEqual(camera.position().x, 0.0f), "A must oppose D movement.");
        require(nearlyEqual(camera.position().z, -1.0f), "S must oppose W movement.");
    }

    void testLevelAndIndirectBatch() {
        lumin::scene::Level level;
        const auto triangle = level.addMesh(makeTriangle("triangle"));
        const auto quad = level.addMesh(makeQuad("quad"));

        level.addModel(triangle, {.position = {-2.0f, 0.0f, 0.0f}, .scale = {2.0f, 1.0f, 0.5f}});
        level.addModel(quad, {.position = {0.0f, 0.0f, 0.0f}});
        level.addModel(triangle, {.position = {2.0f, 0.0f, 0.0f}});

        bool rejectedInvalidHandle = false;
        try {
            level.addModel(lumin::scene::MeshHandle{}, {});
        } catch (const std::out_of_range&) {
            rejectedInvalidHandle = true;
        }
        require(rejectedInvalidHandle, "Level must reject an invalid mesh handle.");

        const lumin::render::ModelBatch batch = lumin::render::ModelRenderer::buildBatch(level);
        require(batch.vertices.size() == 7, "Unique mesh vertices must be packed once.");
        require(batch.indices.size() == 9, "Unique mesh indices must be packed once.");
        require(batch.commands.size() == 3, "One indirect command is required per model.");
        require(batch.objects.size() == 3, "One GPU object record is required per model.");
        require(nearlyEqual(batch.objects[0].normalMatrix[0][0], 0.5f) &&
                    nearlyEqual(batch.objects[0].normalMatrix[1][1], 1.0f) &&
                    nearlyEqual(batch.objects[0].normalMatrix[2][2], 2.0f),
                "Object data must use an inverse-transpose normal matrix for non-uniform scale.");

        const VkDrawIndexedIndirectCommand& first = batch.commands[0];
        const VkDrawIndexedIndirectCommand& second = batch.commands[1];
        const VkDrawIndexedIndirectCommand& third = batch.commands[2];
        require(first.indexCount == 3 && first.firstIndex == 0 && first.vertexOffset == 0 && first.firstInstance == 0,
                "First indirect command has incorrect offsets.");
        require(second.indexCount == 6 && second.firstIndex == 3 && second.vertexOffset == 3 &&
                    second.firstInstance == 0,
                "Second indirect command has incorrect offsets.");
        require(third.indexCount == 3 && third.firstIndex == 0 && third.vertexOffset == 0 && third.firstInstance == 0,
                "Repeated meshes must reuse packed geometry.");
    }

    void testPbrAssetsAndMaterialBatch() {
#if defined(LUMIN_TEST_ASSET_DIR)
        const std::filesystem::path assetDirectory = LUMIN_TEST_ASSET_DIR;
#else
        const std::filesystem::path assetDirectory = "assets";
#endif
        const std::filesystem::path materialDirectory = assetDirectory / "materials" / "aerial_asphalt_01";
        const lumin::scene::PbrTextureSet asphaltTextures{
            .baseColor = materialDirectory / "aerial_asphalt_01_diff_1k.jpg",
            .normal = materialDirectory / "aerial_asphalt_01_nor_gl_1k.png",
            .roughness = materialDirectory / "aerial_asphalt_01_rough_1k.jpg",
            .flipNormalY = true,
        };

        const std::filesystem::path unicodeImagePath =
            std::filesystem::temp_directory_path() / L"lumin_image_loader_\U0001F680.ppm";
        {
            std::ofstream imageFile(unicodeImagePath, std::ios::binary);
            require(static_cast<bool>(imageFile), "The image decoder fixture could not be created.");
            imageFile << "P6\n1 1\n255\n\x20\x80\xff";
        }
        lumin::assets::ImageData unicodePathImage;
        try {
            unicodePathImage = lumin::assets::ImageLoader::load(unicodeImagePath);
        } catch (...) {
            std::error_code cleanupError;
            std::filesystem::remove(unicodeImagePath, cleanupError);
            throw;
        }
        std::error_code cleanupError;
        std::filesystem::remove(unicodeImagePath, cleanupError);
        require(unicodePathImage.width == 1 && unicodePathImage.height == 1 && unicodePathImage.pixels.size() == 4,
                "Image loading must decode RGBA8 data from paths outside the active Windows code page.");

        const lumin::assets::Mesh bunny =
            lumin::assets::ObjLoader::load(assetDirectory / "models" / "stanford-bunny.obj");
        glm::vec2 uvMinimum{std::numeric_limits<float>::max()};
        glm::vec2 uvMaximum{std::numeric_limits<float>::lowest()};
        const bool allBunnyUvsFinite =
            std::all_of(bunny.vertices.begin(), bunny.vertices.end(), [&](const lumin::assets::Vertex& vertex) {
                uvMinimum.x = std::min(uvMinimum.x, vertex.texCoord.x);
                uvMinimum.y = std::min(uvMinimum.y, vertex.texCoord.y);
                uvMaximum.x = std::max(uvMaximum.x, vertex.texCoord.x);
                uvMaximum.y = std::max(uvMaximum.y, vertex.texCoord.y);
                return std::isfinite(vertex.texCoord.x) && std::isfinite(vertex.texCoord.y);
            });
        require(allBunnyUvsFinite && uvMaximum.x - uvMinimum.x > 0.5f && uvMaximum.y - uvMinimum.y > 0.5f,
                "OBJ meshes without vt records must receive finite, non-degenerate texture coordinates.");

#if defined(LUMIN_TEST_SOURCE_DIR)
        const std::filesystem::path testSourceDirectory = LUMIN_TEST_SOURCE_DIR;
#else
        const std::filesystem::path testSourceDirectory = ".";
#endif
        const std::filesystem::path objFixtureDirectory = testSourceDirectory / "tests";
        const lumin::assets::Mesh seamMesh =
            lumin::assets::ObjLoader::load(objFixtureDirectory / "MissingUvSeam.obj.txt");
        float seamMinimum = std::numeric_limits<float>::max();
        float seamMaximum = std::numeric_limits<float>::lowest();
        bool seamUvsFinite = true;
        for (std::size_t index = 0; index < 3; ++index) {
            seamUvsFinite = seamUvsFinite && std::isfinite(seamMesh.vertices[index].texCoord.x) &&
                            std::isfinite(seamMesh.vertices[index].texCoord.y);
            seamMinimum = std::min(seamMinimum, seamMesh.vertices[index].texCoord.x);
            seamMaximum = std::max(seamMaximum, seamMesh.vertices[index].texCoord.x);
        }
        require(seamUvsFinite && seamMaximum - seamMinimum < 0.1f,
                "Generated cylindrical UVs must unwrap triangles that cross the periodic U seam.");

        const lumin::assets::Mesh degenerateMesh =
            lumin::assets::ObjLoader::load(objFixtureDirectory / "MissingUvDegenerate.obj.txt");
        require(std::all_of(degenerateMesh.vertices.begin(), degenerateMesh.vertices.end(),
                            [](const lumin::assets::Vertex& vertex) {
                                return std::isfinite(vertex.texCoord.x) && std::isfinite(vertex.texCoord.y);
                            }),
                "Degenerate OBJ bounds must still produce finite texture coordinates.");

        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("pbr-material"));
        lumin::scene::Material asphalt;
        asphalt.albedo = {0.8f, 0.9f, 1.0f};
        asphalt.roughness = 0.75f;
        asphalt.metallic = 0.2f;
        asphalt.textureScale = 3.0f;
        asphalt.textures = asphaltTextures;
        const auto first = level.addModel(mesh, {}, asphalt);
        level.addModel(mesh, {}, asphalt);
        level.addModel(mesh);

        const lumin::render::ModelBatch batch = lumin::render::ModelRenderer::buildBatch(level);
        require(nearlyEqual(batch.objects[0].baseColorMetallic.w, 0.2f) &&
                    nearlyEqual(batch.objects[0].materialParameters.x, 0.75f) &&
                    nearlyEqual(batch.objects[0].materialParameters.y, 3.0f) &&
                    nearlyEqual(batch.objects[0].materialParameters.z, 1.0f) &&
                    nearlyEqual(batch.objects[0].materialParameters.w, -1.0f),
                "PBR material factors and texture layer must be packed into GPU object data.");
        require(nearlyEqual(batch.objects[1].materialParameters.z, 1.0f) &&
                    nearlyEqual(batch.objects[2].materialParameters.z, 0.0f),
                "Matching PBR materials must share a texture-array layer and defaults must use layer zero.");

        const std::uint64_t scalarTopologyRevision = level.topologyRevision();
        asphalt.metallic = 0.4f;
        require(level.setModelMaterial(first, asphalt) && level.topologyRevision() == scalarTopologyRevision,
                "Scalar PBR changes must not rebuild model topology.");
        const std::uint64_t normalConventionTopologyRevision = level.topologyRevision();
        asphalt.textures->flipNormalY = false;
        require(level.setModelMaterial(first, asphalt) && level.topologyRevision() == normalConventionTopologyRevision,
                "Normal-map convention changes must update object data without rebuilding texture images.");
        const std::uint64_t textureTopologyRevision = level.topologyRevision();
        asphalt.textures->roughness = asphaltTextures.normal;
        require(level.setModelMaterial(first, asphalt) && level.topologyRevision() > textureTopologyRevision,
                "Changing PBR texture paths must rebuild texture-array resources.");
    }

    struct ActorCounters {
        int spawned = 0;
        int ticks = 0;
        int destroyed = 0;
    };

    class ProbeActor final : public lumin::scene::Actor {
    public:
        explicit ProbeActor(std::shared_ptr<ActorCounters> counters, bool spawnInTick = false,
                            bool spawnInOnSpawn = false)
            : counters_(std::move(counters)), spawnInTick_(spawnInTick), spawnInOnSpawn_(spawnInOnSpawn) {
        }

        void onSpawn(lumin::scene::Level& level) override {
            ++counters_->spawned;
            if (spawnInOnSpawn_) {
                spawnInOnSpawn_ = false;
                for (int index = 0; index < 2; ++index) {
                    level.spawnActor<ProbeActor>(counters_);
                }
            }
        }

        void onDestroy(lumin::scene::Level&) override {
            ++counters_->destroyed;
        }

        void tick(lumin::scene::Level& level, float) override {
            ++counters_->ticks;
            if (spawnInTick_) {
                spawnInTick_ = false;
                deferredChild_ = level.spawnActor<ProbeActor>(counters_);
                level.destroyActor(deferredChild_);
                for (int index = 0; index < 64; ++index) {
                    level.spawnActor<ProbeActor>(counters_);
                }
                destroy();
            }
        }

    private:
        std::shared_ptr<ActorCounters> counters_;
        bool spawnInTick_ = false;
        bool spawnInOnSpawn_ = false;
        lumin::scene::ActorHandle deferredChild_{};
    };

    class MovingModelActor final : public lumin::scene::Actor {
    public:
        void onSpawn(lumin::scene::Level& level) override {
            lumin::assets::Mesh mesh = makeTriangle("moving-actor");
            attachModel(level.addMesh(std::move(mesh)));
        }

        void onTick(float) override {
            auto next = transform();
            next.position.x += 1.0f;
            setTransform(next);
        }
    };

    struct NestedDestroyState {
        lumin::scene::ActorHandle root{};
        lumin::scene::ActorHandle victim{};
        lumin::scene::ActorHandle child{};
        int rootDestroyed = 0;
        int victimDestroyed = 0;
        int childSpawned = 0;
        bool recursiveDestroyRejected = false;
        bool childRejectedStaleRoot = false;
        bool victimRejectedStaleRoot = false;
    };

    class ReverseDestroyActor final : public lumin::scene::Actor {
    public:
        explicit ReverseDestroyActor(std::shared_ptr<NestedDestroyState> state) : state_(std::move(state)) {
        }

        void onSpawn(lumin::scene::Level& level) override {
            ++state_->childSpawned;
            state_->childRejectedStaleRoot = !level.destroyActor(state_->root);
        }

    private:
        std::shared_ptr<NestedDestroyState> state_;
    };

    class NestedVictimActor final : public lumin::scene::Actor {
    public:
        explicit NestedVictimActor(std::shared_ptr<NestedDestroyState> state) : state_(std::move(state)) {
        }

        void onDestroy(lumin::scene::Level& level) override {
            ++state_->victimDestroyed;
            state_->victimRejectedStaleRoot = !level.destroyActor(state_->root);
        }

    private:
        std::shared_ptr<NestedDestroyState> state_;
    };

    class RecursiveDestroyActor final : public lumin::scene::Actor {
    public:
        explicit RecursiveDestroyActor(std::shared_ptr<NestedDestroyState> state) : state_(std::move(state)) {
        }

        void onDestroy(lumin::scene::Level& level) override {
            ++state_->rootDestroyed;
            state_->recursiveDestroyRejected = !level.destroyActor(handle());
            level.destroyActor(state_->victim);
            state_->child = level.spawnActor<ReverseDestroyActor>(state_);
        }

    private:
        std::shared_ptr<NestedDestroyState> state_;
    };

    struct ShutdownState {
        int spawnedOnSpawn = 0;
        int spawnedDestructors = 0;
        bool releasedBeforeDestructor = true;
    };

    class ShutdownSpawnedActor final : public lumin::scene::Actor {
    public:
        explicit ShutdownSpawnedActor(std::shared_ptr<ShutdownState> state) : state_(std::move(state)) {
        }

        ~ShutdownSpawnedActor() override {
            ++state_->spawnedDestructors;
            state_->releasedBeforeDestructor =
                state_->releasedBeforeDestructor && level() == nullptr && !handle().isValid();
        }

        void onSpawn(lumin::scene::Level&) override {
            ++state_->spawnedOnSpawn;
        }

    private:
        std::shared_ptr<ShutdownState> state_;
    };

    class ShutdownPassiveActor final : public lumin::scene::Actor {};

    class ShutdownSpawnerActor final : public lumin::scene::Actor {
    public:
        explicit ShutdownSpawnerActor(std::shared_ptr<ShutdownState> state) : state_(std::move(state)) {
        }

        void onDestroy(lumin::scene::Level& level) override {
            level.spawnActor<ShutdownSpawnedActor>(state_);
        }

    private:
        std::shared_ptr<ShutdownState> state_;
    };

    class ThrowingDestroyActor final : public lumin::scene::Actor {
    public:
        void onDestroy(lumin::scene::Level&) override {
            throw std::runtime_error("expected destroy callback failure");
        }
    };

    struct DrainState {
        int throwingCallbacks = 0;
        int modeledCallbacks = 0;
    };

    class BatchThrowingActor final : public lumin::scene::Actor {
    public:
        BatchThrowingActor(std::shared_ptr<DrainState> state, const char* message)
            : state_(std::move(state)), message_(message) {
        }

        void onDestroy(lumin::scene::Level&) override {
            ++state_->throwingCallbacks;
            throw std::runtime_error(message_);
        }

    private:
        std::shared_ptr<DrainState> state_;
        const char* message_ = nullptr;
    };

    class BatchModeledActor final : public lumin::scene::Actor {
    public:
        BatchModeledActor(std::shared_ptr<DrainState> state, lumin::scene::MeshHandle mesh)
            : state_(std::move(state)), mesh_(mesh) {
        }

        void onSpawn(lumin::scene::Level&) override {
            attachModel(mesh_);
        }

        void onDestroy(lumin::scene::Level&) override {
            ++state_->modeledCallbacks;
        }

    private:
        std::shared_ptr<DrainState> state_;
        lumin::scene::MeshHandle mesh_{};
    };

    class BatchDestroyController final : public lumin::scene::Actor {
    public:
        explicit BatchDestroyController(std::array<lumin::scene::ActorHandle, 3> targets) : targets_(targets) {
        }

        void onTick(lumin::scene::Level& level, float) override {
            if (queued_) {
                return;
            }
            queued_ = true;
            for (const lumin::scene::ActorHandle target : targets_) {
                level.destroyActor(target);
            }
            destroy();
        }

    private:
        std::array<lumin::scene::ActorHandle, 3> targets_{};
        bool queued_ = false;
    };

    void testActorLifecycleAndDeferredChanges() {
        lumin::scene::Level level;
        const auto counters = std::make_shared<ActorCounters>();
        const lumin::scene::ActorHandle root = level.spawnActor<ProbeActor>(counters, true, true);
        require(level.actorCount() == 3 && counters->spawned == 3,
                "Actor spawn and onSpawn-generated actors must activate immediately.");

        level.tick(1.0f / 60.0f);
        require(!level.isActorAlive(root), "An actor destroyed during tick must be removed after the tick.");
        require(level.actorCount() == 66, "Deferred burst spawn must commit without invalidating iteration.");
        require(level.pendingActorCount() == 0, "Deferred actor queue must be empty after tick commit.");
        require(counters->spawned == 67, "All deferred actors must receive onSpawn exactly once.");
        require(counters->destroyed == 1, "The destroyed actor must receive onDestroy exactly once.");

        const int ticksAfterFirstFrame = counters->ticks;
        level.tick(1.0f / 60.0f);
        require(counters->ticks == ticksAfterFirstFrame + 66,
                "Actors spawned during tick must first tick on the following frame.");
    }

    void testNestedDestroyCallbacks() {
        lumin::scene::Level level;
        const auto state = std::make_shared<NestedDestroyState>();
        state->victim = level.spawnActor<NestedVictimActor>(state);
        state->root = level.spawnActor<RecursiveDestroyActor>(state);

        require(level.destroyActor(state->root), "Direct actor destruction must be accepted.");
        require(state->rootDestroyed == 1 && state->victimDestroyed == 1,
                "Nested destroy callbacks must run exactly once.");
        require(state->recursiveDestroyRejected && state->childRejectedStaleRoot && state->victimRejectedStaleRoot,
                "Recursive and stale ActorHandle destruction must be rejected.");
        require(!level.isActorAlive(state->root) && !level.isActorAlive(state->victim) &&
                    level.isActorAlive(state->child),
                "Nested callback changes must commit without retaining destroyed actors.");

        const auto throwing = level.spawnActor<ThrowingDestroyActor>();
        bool destroyErrorPropagated = false;
        try {
            level.destroyActor(throwing);
        } catch (const std::runtime_error&) {
            destroyErrorPropagated = true;
        }
        require(destroyErrorPropagated && !level.isActorAlive(throwing),
                "Explicit destroy must propagate callback errors after releasing the actor.");
    }

    void testLevelDestructorSpawnCleanup() {
        const auto state = std::make_shared<ShutdownState>();
        {
            lumin::scene::Level level;
            level.spawnActor<ShutdownPassiveActor>();
            level.spawnActor<ShutdownSpawnerActor>(state);
        }
        require(state->spawnedOnSpawn == 0 && state->spawnedDestructors == 1 && state->releasedBeforeDestructor,
                "Shutdown-spawned actors must be cancelled and detached even when reusing an earlier slot.");
    }

    void testActorOwnershipIsolation() {
        lumin::scene::Level firstLevel;
        lumin::scene::Level secondLevel;
        const auto firstHandle = firstLevel.spawnActor<ShutdownPassiveActor>();
        const auto secondHandle = secondLevel.spawnActor<ShutdownPassiveActor>();
        require(firstHandle == secondHandle, "Fresh Levels must exercise colliding actor handle values.");

        lumin::scene::Actor* secondActor = secondLevel.actor(secondHandle);
        require(secondActor != nullptr && !firstLevel.destroyActor(*secondActor),
                "Actor-reference destruction must reject an Actor owned by another Level.");
        require(firstLevel.isActorAlive(firstHandle) && secondLevel.isActorAlive(secondHandle),
                "A cross-Level handle collision must not destroy either Actor.");
    }

    void testFlushDrainsAfterMultipleDestroyErrors() {
        lumin::scene::Level level;
        const auto state = std::make_shared<DrainState>();
        const auto mesh = level.addMesh(makeTriangle("drain-modeled-actor"));
        const auto first = level.spawnActor<BatchThrowingActor>(state, "first destroy failure");
        const auto second = level.spawnActor<BatchThrowingActor>(state, "second destroy failure");
        const auto modeled = level.spawnActor<BatchModeledActor>(state, mesh);
        const auto controller =
            level.spawnActor<BatchDestroyController>(std::array<lumin::scene::ActorHandle, 3>{first, second, modeled});

        bool firstErrorPropagated = false;
        try {
            level.tick(0.0f);
        } catch (const std::runtime_error& error) {
            firstErrorPropagated = std::string{error.what()} == "first destroy failure";
        }
        require(firstErrorPropagated, "Actor flush must rethrow the first callback error after draining.");
        require(state->throwingCallbacks == 2 && state->modeledCallbacks == 1,
                "Every queued onDestroy callback must run despite earlier failures.");
        require(level.actorCount() == 0 && level.pendingActorCount() == 0 && level.models().empty(),
                "Actor flush must drain all actors, pending states, and trailing models after callback errors.");
        require(!level.isActorAlive(first) && !level.isActorAlive(second) && !level.isActorAlive(modeled) &&
                    !level.isActorAlive(controller),
                "All handles queued in the failing destroy batch must be stale after flush.");
    }

    void testTerrainGenerationAndHeight() {
        lumin::scene::TerrainDesc description;
        description.resolutionX = 4;
        description.resolutionZ = 2;
        description.sizeX = 4.0f;
        description.sizeZ = 2.0f;
        description.heightFunction = [](float x, float z) {
            return x * 0.5f + z * 0.25f;
        };
        lumin::scene::Terrain terrain(description);

        const lumin::assets::Mesh& mesh = terrain.mesh();
        require(mesh.vertices.size() == 15, "Terrain must generate one vertex per grid sample.");
        require(mesh.indices.size() == 48, "Terrain must generate six indices per grid quad.");
        require(std::all_of(mesh.vertices.begin(), mesh.vertices.end(),
                            [](const lumin::assets::Vertex& vertex) {
                                return nearlyEqual(glm::length(vertex.normal), 1.0f);
                            }),
                "Terrain vertices must have normalized normals.");
        require(nearlyEqual(terrain.heightAt(0.0f, 0.0f), 0.0f),
                "Terrain height queries must bilinearly interpolate generated samples.");
        require(nearlyEqual(terrain.heightAt(0.37f, -0.26f), 0.37f * 0.5f - 0.26f * 0.25f),
                "Terrain height queries must interpolate correctly away from grid samples.");
        require(nearlyEqual(terrain.heightAt(-10.0f, -10.0f), terrain.heightAt(-2.0f, -1.0f)),
                "Terrain height queries must clamp to the generated bounds.");

        const std::uint64_t initialRevision = terrain.revision();
        terrain.setHeightSample(2, 1, 3.0f);
        require(terrain.revision() > initialRevision && nearlyEqual(terrain.heightAt(0.0f, 0.0f), 3.0f),
                "Editing a terrain sample must update height queries and revision.");

        lumin::scene::TerrainDesc throwingDescription;
        throwingDescription.resolution = 1;
        throwingDescription.heightFunction = [](float, float) -> float {
            throw std::runtime_error("expected height function failure");
        };
        bool heightErrorPropagated = false;
        try {
            lumin::scene::Terrain throwingTerrain(throwingDescription);
        } catch (const std::runtime_error&) {
            heightErrorPropagated = true;
        }
        require(heightErrorPropagated, "Terrain height-function exceptions must propagate to the caller.");
    }

    void testStableModelAndMeshHandles() {
        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("stable-models"));
        const auto first = level.addModel(mesh, {.position = {1.0f, 0.0f, 0.0f}});
        const auto second = level.addModel(mesh, {.position = {2.0f, 0.0f, 0.0f}});
        require(level.removeModel(first), "A live model handle must be removable.");
        require(nearlyEqual(level.model(second).transform.position.x, 2.0f),
                "Dense model compaction must preserve other stable handles.");
        require(!level.setModelTransform(first, {}) && !level.removeModel(first),
                "A stale model handle must not operate on a compacted model.");
        bool staleModelRejected = false;
        try {
            static_cast<void>(level.model(first));
        } catch (const std::out_of_range&) {
            staleModelRejected = true;
        }
        require(staleModelRejected, "Reading a stale model handle must fail.");

        const auto replacement = level.addModel(mesh, {.position = {3.0f, 0.0f, 0.0f}});
        require(replacement.index == first.index && replacement.generation != first.generation,
                "Reused model slots must advance generation.");
        require(!level.setModelMaterial(first, {}) && nearlyEqual(level.model(replacement).transform.position.x, 3.0f),
                "A stale generation must never redirect to a replacement model.");
        require(level.models().size() == 2, "Renderer-facing models must remain a continuous active list.");

        lumin::scene::Level meshLevel;
        const auto oldMesh = meshLevel.addMesh(makeTriangle("old-mesh"));
        const auto oldModel = meshLevel.addModel(oldMesh);
        require(meshLevel.removeModel(oldModel) && meshLevel.removeMesh(oldMesh),
                "An unreferenced mesh must release its slot.");
        const auto replacementMesh = meshLevel.addMesh(makeQuad("replacement-mesh"));
        require(replacementMesh.index == oldMesh.index && replacementMesh.generation != oldMesh.generation &&
                    meshLevel.meshes().size() == 1,
                "Released mesh storage must be reused with a new generation.");
        require(!meshLevel.replaceMesh(oldMesh, makeTriangle("stale-replace")),
                "A stale mesh handle must not replace reused geometry.");
        meshLevel.addModel(replacementMesh);
        const lumin::render::ModelBatch batch = lumin::render::ModelRenderer::buildBatch(meshLevel);
        require(batch.vertices.size() == 4 && batch.indices.size() == 6 && batch.commands[0].indexCount == 6,
                "Mesh-slot reuse must preserve the renderer's handle-to-mesh index contract.");
    }

    void testModelRevisionAndTerrainActor() {
        lumin::scene::Level level;
        lumin::assets::Mesh mesh = makeTriangle("revision-triangle");
        const lumin::scene::MeshHandle meshHandle = level.addMesh(std::move(mesh));
        const lumin::scene::ModelHandle modelHandle = level.addModel(meshHandle);
        const std::uint64_t topologyRevision = level.topologyRevision();
        const std::uint64_t modelRevision = level.modelRevision();

        lumin::scene::Transform moved;
        moved.position = {2.0f, 3.0f, 4.0f};
        require(level.setModelTransform(modelHandle, moved), "Model transform update must accept a valid handle.");
        require(level.modelRevision() > modelRevision && level.topologyRevision() == topologyRevision,
                "Model transforms must advance model revision without changing topology revision.");
        require(nearlyEqual(level.model(modelHandle).transform.matrix()[3][0], 2.0f),
                "Updated model transform must be exposed to the renderer-facing model list.");

        const std::uint64_t materialRevision = level.modelRevision();
        require(level.setModelMaterial(modelHandle, lumin::scene::Material{{0.1f, 0.2f, 0.3f}, 0.2f}),
                "Model material update must accept a valid handle.");
        require(level.modelRevision() > materialRevision, "Material updates must advance model revision.");

        const auto movingActorHandle = level.spawnActor<MovingModelActor>();
        const auto* movingActor = level.actor(movingActorHandle);
        require(movingActor != nullptr && movingActor->modelHandle() != lumin::scene::InvalidModelHandle,
                "An Actor must be able to attach a model during onSpawn.");
        const std::uint64_t actorModelRevision = level.modelRevision();
        level.tick(0.0f);
        require(level.modelRevision() > actorModelRevision &&
                    nearlyEqual(level.model(movingActor->modelHandle()).transform.position.x, 1.0f),
                "Actor tick transforms must update the renderer-facing model and its revision.");

        lumin::scene::TerrainDesc terrainDescription;
        terrainDescription.resolution = 2;
        terrainDescription.sizeX = 4.0f;
        terrainDescription.sizeZ = 4.0f;
        const auto terrainActorHandle = level.spawnActor<lumin::scene::TerrainActor>(terrainDescription);
        auto* terrainActor = level.actor(terrainActorHandle);
        require(terrainActor != nullptr && terrainActor->modelHandle() != lumin::scene::InvalidModelHandle,
                "TerrainActor must attach a renderable model during onSpawn.");
        const std::uint64_t terrainTopologyRevision = level.topologyRevision();
        auto* mutableTerrainActor = dynamic_cast<lumin::scene::TerrainActor*>(terrainActor);
        require(mutableTerrainActor != nullptr, "Terrain actor handle must resolve to TerrainActor.");
        mutableTerrainActor->terrain().setHeightSample(1, 1, 2.0f);
        level.tick(0.0f);
        require(level.topologyRevision() > terrainTopologyRevision,
                "TerrainActor mesh edits must advance Level topology revision.");
        const lumin::scene::MeshHandle terrainMesh = mutableTerrainActor->terrainMeshHandle();
        require(nearlyEqual(level.mesh(terrainMesh).vertices[4].position.y, 2.0f),
                "TerrainActor mesh replacement must publish edited vertex content to Level.");

        lumin::scene::Level terrainLevel;
        const auto firstTerrainHandle = terrainLevel.spawnActor<lumin::scene::TerrainActor>(terrainDescription);
        auto* firstTerrain = dynamic_cast<lumin::scene::TerrainActor*>(terrainLevel.actor(firstTerrainHandle));
        require(firstTerrain != nullptr, "First TerrainActor must spawn.");
        const lumin::scene::MeshHandle firstTerrainMesh = firstTerrain->terrainMeshHandle();
        const std::size_t terrainSlotCount = terrainLevel.meshes().size();
        require(terrainLevel.destroyActor(firstTerrainHandle) && !terrainLevel.isMeshAlive(firstTerrainMesh),
                "Destroying TerrainActor must release its procedural mesh.");
        const auto secondTerrainHandle = terrainLevel.spawnActor<lumin::scene::TerrainActor>(terrainDescription);
        auto* secondTerrain = dynamic_cast<lumin::scene::TerrainActor*>(terrainLevel.actor(secondTerrainHandle));
        require(secondTerrain != nullptr && secondTerrain->terrainMeshHandle().index == firstTerrainMesh.index &&
                    secondTerrain->terrainMeshHandle().generation != firstTerrainMesh.generation &&
                    terrainLevel.meshes().size() == terrainSlotCount,
                "TerrainActor mesh slots must be reused without accumulating geometry resources.");
    }

} // namespace

int main() {
    try {
        testCameraMovement();
        testLevelAndIndirectBatch();
        testPbrAssetsAndMaterialBatch();
        testActorLifecycleAndDeferredChanges();
        testNestedDestroyCallbacks();
        testLevelDestructorSpawnCleanup();
        testActorOwnershipIsolation();
        testFlushDrainsAfterMultipleDestroyErrors();
        testTerrainGenerationAndHeight();
        testStableModelAndMeshHandles();
        testModelRevisionAndTerrainActor();
        std::cout << "CameraLevel PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CameraLevel FAIL: " << error.what() << '\n';
        return 1;
    }
}

#include "render/world/RenderWorld.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

    using lumin::render::world::RenderWorldCache;
    using lumin::render::world::SceneChangeMask;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] lumin::assets::Mesh makeTriangle(std::string name, float xOffset = 0.0f) {
        lumin::assets::Mesh mesh;
        mesh.name = std::move(name);
        mesh.vertices = {
            {{xOffset + 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{xOffset + 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{xOffset + 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    void testSharedMeshIsCopiedOnce() {
        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("shared"));
        const auto first = level.addModel(mesh);
        lumin::scene::Material secondMaterial;
        secondMaterial.albedo = {0.2f, 0.4f, 0.8f};
        const auto second = level.addModel(mesh, {.position = {2.0f, 0.0f, 0.0f}}, secondMaterial);

        RenderWorldCache cache;
        const auto delta = cache.sync(level);
        require(delta.changes == SceneChangeMask::All, "The first sync must initialize every render-world domain.");
        require(delta.snapshot != nullptr, "The first sync must publish a snapshot.");
        require(delta.snapshot->meshes().size() == 1, "A mesh referenced by two models must be copied exactly once.");
        require(delta.snapshot->instances().size() == 2, "Every live model must produce one render instance.");
        require(delta.snapshot->instances()[0].meshIndex == 0 && delta.snapshot->instances()[1].meshIndex == 0,
                "Shared mesh references must resolve to the same compact mesh index.");
        require(delta.snapshot->findInstance(first) != nullptr && delta.snapshot->findInstance(second) != nullptr,
                "Stable model handles must resolve inside the snapshot.");
        require(delta.snapshot->findInstance(second)->model.material.albedo == secondMaterial.albedo,
                "The snapshot must own a complete material copy.");
        require(delta.snapshot->sourceRevision() == level.revision() &&
                    delta.snapshot->sourceTopologyRevision() == level.topologyRevision() &&
                    delta.snapshot->sourceModelRevision() == level.modelRevision() &&
                    delta.snapshot->sourceLightingRevision() == level.lightingRevision() &&
                    delta.snapshot->sourceAtmosphereRevision() == level.atmosphereRevision(),
                "The snapshot must record all source revisions.");
    }

    void testTransformChangeDoesNotReportTopology() {
        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("moving"));
        const auto model = level.addModel(mesh);
        RenderWorldCache cache;
        const auto before = cache.sync(level).snapshot;

        lumin::scene::Transform moved;
        moved.position = {4.0f, 5.0f, 6.0f};
        require(level.setModelTransform(model, moved), "A live model transform update must succeed.");
        const auto delta = cache.sync(level);

        require(delta.changes == SceneChangeMask::TransformOrMaterial,
                "A transform-only edit must not report geometry or instance topology changes.");
        require(delta.snapshot->sourceTopologyRevision() == before->sourceTopologyRevision(),
                "A transform-only edit must preserve the source topology revision.");
        require(&delta.snapshot->meshes()[0] == &before->meshes()[0],
                "A transform-only edit must share immutable renderer-owned geometry storage.");
        require(delta.snapshot->findInstance(model)->model.transform.position == moved.position,
                "The new snapshot must contain the updated transform.");
    }

    void testTopologyAndGeometryChangesAreSeparated() {
        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("topology"));
        level.addModel(mesh);
        RenderWorldCache cache;
        static_cast<void>(cache.sync(level));

        const auto addedModel = level.addModel(mesh, {.position = {2.0f, 0.0f, 0.0f}});
        const auto topologyDelta = cache.sync(level);
        require(topologyDelta.changes == SceneChangeMask::InstanceTopology,
                "Adding an instance of an uploaded mesh must report topology without geometry changes.");
        require(topologyDelta.snapshot->findInstance(addedModel) != nullptr,
                "The topology snapshot must include the new stable model handle.");

        require(level.replaceMesh(mesh, makeTriangle("topology", 3.0f)), "A live mesh replacement must succeed.");
        const auto geometryDelta = cache.sync(level);
        require(geometryDelta.changes == SceneChangeMask::Geometry,
                "Replacing mesh contents must report a geometry-only change.");
    }

    void testOldSnapshotRemainsImmutable() {
        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("old", 1.0f));
        const auto model = level.addModel(mesh);
        RenderWorldCache cache;
        const auto oldSnapshot = cache.sync(level).snapshot;
        const float oldVertexX = oldSnapshot->meshes()[0].mesh.vertices[0].position.x;
        const glm::vec3 oldAlbedo = oldSnapshot->findInstance(model)->model.material.albedo;

        lumin::scene::Material replacementMaterial;
        replacementMaterial.albedo = {0.1f, 0.9f, 0.3f};
        require(level.setModelMaterial(model, replacementMaterial), "A live model material update must succeed.");
        require(level.replaceMesh(mesh, makeTriangle("new", 9.0f)), "A live mesh replacement must succeed.");
        const auto delta = cache.sync(level);

        require(delta.has(SceneChangeMask::Geometry) && delta.has(SceneChangeMask::TransformOrMaterial),
                "Combined mesh and material edits must preserve both change categories.");
        require(delta.snapshot != oldSnapshot, "A changed scene must publish a new snapshot object.");
        require(oldSnapshot->meshes()[0].mesh.name == "old" &&
                    oldSnapshot->meshes()[0].mesh.vertices[0].position.x == oldVertexX,
                "A later geometry sync must not mutate an older shared snapshot.");
        require(oldSnapshot->findInstance(model)->model.material.albedo == oldAlbedo,
                "A later material sync must not mutate an older shared snapshot.");
        require(delta.snapshot->meshes()[0].mesh.name == "new" &&
                    delta.snapshot->findInstance(model)->model.material.albedo == replacementMaterial.albedo,
                "The new snapshot must contain both updated values.");
    }

    void testUnchangedSyncReturnsNoneAndReusesSnapshot() {
        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("unchanged"));
        level.addModel(mesh);
        RenderWorldCache cache;
        const auto first = cache.sync(level).snapshot;
        const auto unchanged = cache.sync(level);

        require(unchanged.changes == SceneChangeMask::None && !unchanged.changed(),
                "Synchronizing an unchanged scene must return SceneChangeMask::None.");
        require(unchanged.snapshot == first && cache.snapshot() == first,
                "Synchronizing an unchanged scene must reuse the current immutable snapshot.");

        const auto unusedMesh = level.addMesh(makeTriangle("unused"));
        static_cast<void>(unusedMesh);
        const auto unrelatedGeometry = cache.sync(level);
        require(unrelatedGeometry.changes == SceneChangeMask::None,
                "Adding an unreferenced mesh must not invalidate the render-world snapshot contents.");
        require(unrelatedGeometry.snapshot->sourceTopologyRevision() == level.topologyRevision(),
                "A no-op render delta must still capture the latest source revisions.");
        require(&unrelatedGeometry.snapshot->meshes()[0] == &first->meshes()[0],
                "An unreferenced mesh edit must preserve immutable referenced geometry storage.");
    }

    void testCacheDoesNotConfuseIndependentLevels() {
        lumin::scene::Level firstLevel;
        const auto firstMesh = firstLevel.addMesh(makeTriangle("first-level", 1.0f));
        firstLevel.addModel(firstMesh);

        lumin::scene::Level secondLevel;
        const auto secondMesh = secondLevel.addMesh(makeTriangle("second-level", 7.0f));
        secondLevel.addModel(secondMesh);
        require(firstLevel.revision() == secondLevel.revision() &&
                    firstLevel.topologyRevision() == secondLevel.topologyRevision() &&
                    firstLevel.modelRevision() == secondLevel.modelRevision(),
                "The test setup must produce colliding source revisions.");

        RenderWorldCache cache;
        const auto firstSnapshot = cache.sync(firstLevel).snapshot;
        const auto switched = cache.sync(secondLevel);
        require(switched.changes == SceneChangeMask::All && switched.snapshot != firstSnapshot,
                "Switching Level objects must initialize a new snapshot despite colliding revisions.");
        require(switched.snapshot->meshes()[0].mesh.name == "second-level",
                "The cache must publish geometry from the newly selected Level.");
    }

    void testEnvironmentChangesHaveIndependentDeltaBits() {
        lumin::scene::Level level;
        const auto mesh = level.addMesh(makeTriangle("environment"));
        level.addModel(mesh);
        RenderWorldCache cache;
        const auto before = cache.sync(level).snapshot;

        lumin::scene::DirectionalLight sun = level.environment().sun;
        sun.color = {0.7f, 0.8f, 1.0f};
        level.setSun(sun);
        const auto lighting = cache.sync(level);
        require(lighting.changes == SceneChangeMask::Lighting,
                "A sun-only edit must report an independent lighting change.");
        require(lighting.snapshot->environment().sun.color == sun.color &&
                    before->environment().sun.color != sun.color,
                "Environment values must be copied into immutable snapshots.");

        lumin::scene::AtmosphereParameters atmosphere = level.environment().atmosphere;
        atmosphere.topRadiusKm += 10.0f;
        level.setAtmosphere(atmosphere);
        const auto atmosphereDelta = cache.sync(level);
        require(atmosphereDelta.changes == SceneChangeMask::Atmosphere,
                "An atmosphere-only edit must report an independent atmosphere change.");
        require(&atmosphereDelta.snapshot->meshes()[0] == &lighting.snapshot->meshes()[0],
                "Environment edits must preserve renderer-owned immutable geometry storage.");
    }

} // namespace

int main() {
    try {
        testSharedMeshIsCopiedOnce();
        testTransformChangeDoesNotReportTopology();
        testTopologyAndGeometryChangesAreSeparated();
        testOldSnapshotRemainsImmutable();
        testUnchangedSyncReturnsNoneAndReusesSnapshot();
        testCacheDoesNotConfuseIndependentLevels();
        testEnvironmentChangesHaveIndependentDeltaBits();
        std::cout << "RenderWorld PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RenderWorld FAIL: " << error.what() << '\n';
        return 1;
    }
}

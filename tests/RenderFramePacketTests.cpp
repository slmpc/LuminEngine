#include "render/core/RenderFramePacket.hpp"
#include "render/pipelines/DefaultRenderPipelines.hpp"

#include "scene/Camera.hpp"
#include "scene/Level.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] lumin::assets::Mesh makeTriangle() {
        lumin::assets::Mesh mesh;
        mesh.name = "packet-triangle";
        mesh.vertices = {
            {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    void testPacketOwnsImmutableSnapshots() {
        using namespace lumin;
        scene::Level level;
        const scene::MeshHandle mesh = level.addMesh(makeTriangle());
        const scene::ModelHandle model = level.addModel(mesh);
        scene::Camera camera;

        render::RenderSettings settings;
        render::core::UiDrawPacket ui;
        ui.displayWidth = 640.0f;
        ui.displayHeight = 360.0f;
        ui.vertices.push_back({});

        render::core::RenderFramePacketBuilder builder;
        render::core::RenderFramePacket packet =
            builder.build(level, camera, render::pipelines::makeDefaultRenderSettingsSnapshot(settings), std::move(ui),
                          render::core::SurfaceState{.windowExtent = {1280, 720}, .viewportExtent = {640, 360}});
        require(packet.isValid() && packet.world->instances().size() == 1 && packet.ui.vertices.size() == 1,
                "A packet must own complete scene and UI data.");

        const glm::vec4 packetPosition = packet.camera.position;
        scene::Transform moved;
        moved.position = {4.0f, 2.0f, -1.0f};
        require(level.setModelTransform(model, moved), "The test model must remain editable.");
        camera.setPosition({8.0f, 3.0f, 2.0f});
        settings.toneMapping.exposure = 3.0f;

        require(packet.world->findInstance(model)->model.transform.position != moved.position,
                "Mutating Level after build must not affect an older packet.");
        require(packet.camera.position == packetPosition,
                "Mutating Camera after build must not affect an older packet.");
        require(
            packet.settings.get<render::ToneMappingSettings>(render::pipelines::feature_ids::toneMapping()).exposure ==
                1.0f,
            "Mutating aggregate settings after build must not affect a typed snapshot.");

        const render::core::RenderFramePacket next =
            builder.build(level, camera, render::pipelines::makeDefaultRenderSettingsSnapshot(settings), {},
                          render::core::SurfaceState{.windowExtent = {1280, 720}, .viewportExtent = {640, 360}});
        require(
            next.clientFrame.value == packet.clientFrame.value + 1 &&
                render::world::hasAnyChange(next.sceneChangesHint, render::world::SceneChangeMask::TransformOrMaterial),
            "The builder must publish monotonic identities and a main-thread scene delta hint.");
    }

    void testSuccessfulBaselineComparisonSpansDroppedPackets() {
        using namespace lumin;
        scene::Level level;
        const scene::MeshHandle mesh = level.addMesh(makeTriangle());
        level.addModel(mesh);
        render::world::RenderWorldCache cache;
        const render::world::RenderWorldSnapshotPtr submitted = cache.sync(level).snapshot;

        level.addModel(mesh);
        const render::world::RenderWorldSnapshotPtr dropped = cache.sync(level).snapshot;
        require(render::world::changesBetween(submitted, dropped) == render::world::SceneChangeMask::InstanceTopology,
                "The intermediate packet must contain its topology edit.");

        lumin::scene::DirectionalLight sun = level.environment().sun;
        sun.illuminanceLux *= 0.5f;
        level.setSun(sun);
        const render::world::RenderWorldSnapshotPtr latest = cache.sync(level).snapshot;
        const render::world::SceneChangeMask changes = render::world::changesBetween(submitted, latest);
        require(render::world::hasAnyChange(changes, render::world::SceneChangeMask::InstanceTopology) &&
                    render::world::hasAnyChange(changes, render::world::SceneChangeMask::Lighting),
                "Comparing with the last submitted snapshot must retain changes from a dropped packet.");
    }

} // namespace

int main() {
    try {
        testPacketOwnsImmutableSnapshots();
        testSuccessfulBaselineComparisonSpansDroppedPackets();
        std::cout << "RenderFramePacket PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RenderFramePacket FAIL: " << error.what() << '\n';
        return 1;
    }
}

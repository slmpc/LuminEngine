#include "lumin/render/ModelRenderer.hpp"
#include "lumin/scene/Camera.hpp"
#include "lumin/scene/CameraController.hpp"
#include "lumin/scene/Level.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

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

        level.addModel(triangle, {.position = {-2.0f, 0.0f, 0.0f}});
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

        const VkDrawIndexedIndirectCommand& first = batch.commands[0];
        const VkDrawIndexedIndirectCommand& second = batch.commands[1];
        const VkDrawIndexedIndirectCommand& third = batch.commands[2];
        require(first.indexCount == 3 && first.firstIndex == 0 && first.vertexOffset == 0 && first.firstInstance == 0,
                "First indirect command has incorrect offsets.");
        require(second.indexCount == 6 && second.firstIndex == 3 && second.vertexOffset == 3 &&
                    second.firstInstance == 0,
                "Second indirect command has incorrect offsets.");
        require(third.indexCount == 3 && third.firstIndex == 0 && third.vertexOffset == 0 &&
                    third.firstInstance == 0,
                "Repeated meshes must reuse packed geometry.");
    }

}

int main() {
    try {
        testCameraMovement();
        testLevelAndIndirectBatch();
        std::cout << "CameraLevel PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CameraLevel FAIL: " << error.what() << '\n';
        return 1;
    }
}

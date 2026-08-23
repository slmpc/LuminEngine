#include "render/core/RenderFramePacket.hpp"

#include "scene/Camera.hpp"
#include <limits>
#include <stdexcept>

namespace lumin::render::core {

    bool RenderFramePacket::isValid() const noexcept {
        return world != nullptr && camera.nearPlane > 0.0f && camera.farPlane > camera.nearPlane &&
               surface.viewportExtent.width > 0 && surface.viewportExtent.height > 0;
    }

    RenderFramePacket RenderFramePacketBuilder::build(world::RenderWorldSnapshotPtr world, const scene::Camera& camera,
                                                      RenderSettingsSnapshot settings, SurfaceState surface) {
        if (world == nullptr) {
            throw std::invalid_argument("Render frame packet requires a world snapshot.");
        }
        if (surface.viewportExtent.width == 0 || surface.viewportExtent.height == 0) {
            throw std::invalid_argument("Render frame packet requires a non-empty Viewport extent.");
        }
        if (nextClientFrame_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Render frame packet builder exhausted the client frame range.");
        }

        const float aspectRatio =
            static_cast<float>(surface.viewportExtent.width) / static_cast<float>(surface.viewportExtent.height);
        const glm::mat4 view = camera.viewMatrix();
        const glm::mat4 projection = camera.projectionMatrix(aspectRatio);
        const world::SceneChangeMask sceneChanges = world::changesBetween(worldSnapshot_, world);

        RenderFramePacket packet{
            .clientFrame = ClientFrameId{nextClientFrame_},
            .world = std::move(world),
            .camera =
                CameraFrameData{
                    .view = view,
                    .projection = projection,
                    .viewProjection = projection * view,
                    .previousViewProjection = projection * view,
                    .position = glm::vec4{camera.position(), 1.0f},
                    .forward = glm::vec4{camera.forward(), 0.0f},
                    .right = camera.right(),
                    .up = camera.up(),
                    .fieldOfViewDegrees = camera.fieldOfViewDegrees(),
                    .nearPlane = camera.nearPlane(),
                    .farPlane = camera.farPlane(),
                    .revision = camera.revision(),
                    .jitter = glm::vec2{0.0f},
                    .cutEpoch = camera.cutEpoch(),
                },
            .settings = std::move(settings),
            .surface = surface,
            .sceneChangesHint = sceneChanges,
        };
        worldSnapshot_ = packet.world;
        ++nextClientFrame_;
        return packet;
    }

    world::RenderWorldSnapshotPtr RenderFramePacketBuilder::worldSnapshot() const noexcept {
        return worldSnapshot_;
    }

} // namespace lumin::render::core

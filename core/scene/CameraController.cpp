#include "scene/CameraController.hpp"

#include "scene/Camera.hpp"

#include <algorithm>

namespace lumin::scene {

    void CameraController::update(Camera& camera, const CameraInput& input, float deltaSeconds) {
        const float distance = camera.moveSpeed() * std::max(deltaSeconds, 0.0f);
        const glm::vec3 offset = camera.forward() * input.forward + camera.right() * input.right +
                                 glm::vec3{0.0f, 1.0f, 0.0f} * input.up;
        camera.translate(offset * distance);
    }

}

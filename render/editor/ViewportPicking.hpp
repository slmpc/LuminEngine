#pragma once

#include <optional>

#include <glm/vec3.hpp>

#include "scene/Camera.hpp"
#include "scene/Level.hpp"

namespace lumin::editor {

    struct ViewportRay {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};
    };

    struct ViewportPickResult {
        scene::ModelHandle model;
        float distance = 0.0f;
        glm::vec3 worldPosition{0.0f};
    };

    [[nodiscard]] ViewportRay makeViewportRay(const scene::Camera& camera, float pixelX, float pixelY, float width,
                                              float height);
    [[nodiscard]] std::optional<ViewportPickResult> pickViewportModel(const scene::Level& level,
                                                                      const ViewportRay& ray);

} // namespace lumin::editor

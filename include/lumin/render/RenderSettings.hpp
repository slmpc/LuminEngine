#pragma once

#include <glm/vec3.hpp>

namespace lumin::render {

    struct RenderSettings {
        glm::vec3 cameraPosition{0.0f, 1.25f, 3.5f};
        glm::vec3 lightPosition{2.5f, 3.5f, 2.0f};
        glm::vec3 lightColor{1.0f, 0.95f, 0.85f};
        glm::vec3 materialColor{0.82f, 0.68f, 0.48f};
        float ambientStrength = 0.12f;
        float specularStrength = 0.55f;
        float shininess = 48.0f;
        bool smoothShading = true;
        glm::vec3 sunDirection{-0.45f, -0.8f, -0.35f};
        float exposure = 1.0f;
        bool enableShadows = true;
        bool enableSsao = true;
        bool enableTaa = true;
    };

} // namespace lumin::render

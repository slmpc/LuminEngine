#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace lumin::scene {

    class Camera {
    public:
        [[nodiscard]] const glm::vec3& position() const noexcept;
        [[nodiscard]] float yawDegrees() const noexcept;
        [[nodiscard]] float pitchDegrees() const noexcept;
        [[nodiscard]] float moveSpeed() const noexcept;
        [[nodiscard]] float fieldOfViewDegrees() const noexcept;

        void setPosition(const glm::vec3& position) noexcept;
        void setOrientation(float yawDegrees, float pitchDegrees) noexcept;
        void setMoveSpeed(float speed) noexcept;
        void setFieldOfViewDegrees(float degrees) noexcept;
        void translate(const glm::vec3& offset) noexcept;

        [[nodiscard]] glm::vec3 forward() const;
        [[nodiscard]] glm::vec3 right() const;
        [[nodiscard]] glm::vec3 up() const;
        [[nodiscard]] glm::mat4 viewMatrix() const;
        [[nodiscard]] glm::mat4 projectionMatrix(float aspectRatio) const;

    private:
        glm::vec3 position_{0.0f, 1.25f, 6.0f};
        float yawDegrees_ = -90.0f;
        float pitchDegrees_ = 0.0f;
        float moveSpeed_ = 3.5f;
        float fieldOfViewDegrees_ = 60.0f;
    };

}

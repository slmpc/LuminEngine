#include "scene/Camera.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace lumin::scene {

    const glm::vec3& Camera::position() const noexcept {
        return position_;
    }

    float Camera::yawDegrees() const noexcept {
        return yawDegrees_;
    }

    float Camera::pitchDegrees() const noexcept {
        return pitchDegrees_;
    }

    float Camera::moveSpeed() const noexcept {
        return moveSpeed_;
    }

    float Camera::fieldOfViewDegrees() const noexcept {
        return fieldOfViewDegrees_;
    }

    float Camera::nearPlane() const noexcept {
        return nearPlane_;
    }

    float Camera::farPlane() const noexcept {
        return farPlane_;
    }

    std::uint64_t Camera::revision() const noexcept {
        return revision_;
    }

    std::uint64_t Camera::cutEpoch() const noexcept {
        return cutEpoch_;
    }

    void Camera::setPosition(const glm::vec3& position) noexcept {
        if (position_ == position) {
            return;
        }
        position_ = position;
        ++revision_;
    }

    void Camera::setOrientation(float yawDegrees, float pitchDegrees) noexcept {
        const float clampedPitch = std::clamp(pitchDegrees, -89.0f, 89.0f);
        if (yawDegrees_ == yawDegrees && pitchDegrees_ == clampedPitch) {
            return;
        }
        yawDegrees_ = yawDegrees;
        pitchDegrees_ = clampedPitch;
        ++revision_;
    }

    void Camera::setMoveSpeed(float speed) noexcept {
        moveSpeed_ = std::max(speed, 0.0f);
    }

    void Camera::setFieldOfViewDegrees(float degrees) noexcept {
        const float clampedDegrees = std::clamp(degrees, 1.0f, 120.0f);
        if (fieldOfViewDegrees_ == clampedDegrees) {
            return;
        }
        fieldOfViewDegrees_ = clampedDegrees;
        ++revision_;
    }

    void Camera::setClipPlanes(float nearPlane, float farPlane) {
        if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane <= 0.0f || farPlane <= nearPlane) {
            throw std::invalid_argument("Camera clip planes require 0 < near < far and finite values.");
        }
        if (nearPlane_ == nearPlane && farPlane_ == farPlane) {
            return;
        }
        nearPlane_ = nearPlane;
        farPlane_ = farPlane;
        ++revision_;
    }

    void Camera::translate(const glm::vec3& offset) noexcept {
        if (offset == glm::vec3{0.0f}) {
            return;
        }
        position_ += offset;
        ++revision_;
    }

    void Camera::markCut() noexcept {
        ++cutEpoch_;
        ++revision_;
    }

    glm::vec3 Camera::forward() const {
        const float yaw = glm::radians(yawDegrees_);
        const float pitch = glm::radians(pitchDegrees_);
        return glm::normalize(glm::vec3{
            std::cos(yaw) * std::cos(pitch),
            std::sin(pitch),
            std::sin(yaw) * std::cos(pitch),
        });
    }

    glm::vec3 Camera::right() const {
        // 使用固定世界上方向构造右向量，避免相机俯仰时 WASD 平移发生滚转。
        return glm::normalize(glm::cross(forward(), glm::vec3{0.0f, 1.0f, 0.0f}));
    }

    glm::vec3 Camera::up() const {
        return glm::normalize(glm::cross(right(), forward()));
    }

    glm::mat4 Camera::viewMatrix() const {
        return glm::lookAt(position_, position_ + forward(), up());
    }

    glm::mat4 Camera::projectionMatrix(float aspectRatio) const {
        return glm::perspective(glm::radians(fieldOfViewDegrees_), std::max(aspectRatio, 0.001f), nearPlane_, farPlane_);
    }

} // namespace lumin::scene

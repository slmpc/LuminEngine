#pragma once

#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace lumin::scene {

    class Camera {
    public:
        /** 返回相机位置。 */
        [[nodiscard]] const glm::vec3& position() const noexcept;
        [[nodiscard]] float yawDegrees() const noexcept;
        [[nodiscard]] float pitchDegrees() const noexcept;
        [[nodiscard]] float moveSpeed() const noexcept;
        [[nodiscard]] float fieldOfViewDegrees() const noexcept;
        [[nodiscard]] float nearPlane() const noexcept;
        [[nodiscard]] float farPlane() const noexcept;

        /**
         * 返回影响渲染视图的属性修订号。
         *
         * 普通连续移动也会推进该值；时序算法不得把它直接解释为 camera cut。
         */
        [[nodiscard]] std::uint64_t revision() const noexcept;

        /**
         * 返回显式 camera cut 代数。
         *
         * 切换活动相机、传送或编辑器跳转应调用 markCut()。渲染器只在成功提交后记住已消费的代数。
         */
        [[nodiscard]] std::uint64_t cutEpoch() const noexcept;

        void setPosition(const glm::vec3& position) noexcept;
        void setOrientation(float yawDegrees, float pitchDegrees) noexcept;
        void setMoveSpeed(float speed) noexcept;
        void setFieldOfViewDegrees(float degrees) noexcept;
        void setClipPlanes(float nearPlane, float farPlane);
        void translate(const glm::vec3& offset) noexcept;
        void markCut() noexcept;

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
        float nearPlane_ = 0.05f;
        float farPlane_ = 200.0f;
        std::uint64_t revision_ = 0;
        std::uint64_t cutEpoch_ = 0;
    };

} // namespace lumin::scene

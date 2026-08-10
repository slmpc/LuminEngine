#pragma once

namespace lumin::scene {

    class Camera;

    struct CameraInput {
        float forward = 0.0f;
        float right = 0.0f;
        float up = 0.0f;
        /** 帧内相对鼠标位移；不乘 delta time，避免帧率改变视角旋转速度。 */
        float lookDeltaX = 0.0f;
        float lookDeltaY = 0.0f;
        float lookSensitivity = 0.12f;
    };

    class CameraController {
    public:
        static void update(Camera& camera, const CameraInput& input, float deltaSeconds);
    };

} // namespace lumin::scene

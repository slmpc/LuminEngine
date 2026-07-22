#pragma once

namespace lumin::scene {

    class Camera;

    struct CameraInput {
        float forward = 0.0f;
        float right = 0.0f;
        float up = 0.0f;
    };

    class CameraController {
    public:
        static void update(Camera& camera, const CameraInput& input, float deltaSeconds);
    };

}

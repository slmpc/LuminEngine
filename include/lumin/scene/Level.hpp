#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "lumin/assets/ObjLoader.hpp"

namespace lumin::scene {

    struct MeshHandle {
        std::uint32_t index = std::numeric_limits<std::uint32_t>::max();

        [[nodiscard]] bool isValid() const noexcept;
        friend bool operator==(MeshHandle, MeshHandle) = default;
    };

    struct Transform {
        glm::vec3 position{0.0f};
        glm::vec3 rotationDegrees{0.0f};
        glm::vec3 scale{1.0f};

        [[nodiscard]] glm::mat4 matrix() const;
    };

    struct Material {
        glm::vec3 albedo{0.82f, 0.68f, 0.48f};
        float roughness = 0.45f;
    };

    struct ModelInstance {
        MeshHandle mesh;
        Transform transform;
        Material material;
    };

    class Level {
    public:
        [[nodiscard]] MeshHandle addMesh(assets::Mesh mesh);
        std::uint32_t addModel(MeshHandle mesh, Transform transform = {}, Material material = {});

        [[nodiscard]] const assets::Mesh& mesh(MeshHandle handle) const;
        [[nodiscard]] const std::vector<assets::Mesh>& meshes() const noexcept;
        [[nodiscard]] const std::vector<ModelInstance>& models() const noexcept;

    private:
        std::vector<assets::Mesh> meshes_;
        std::vector<ModelInstance> models_;
    };

}

#include "lumin/scene/Level.hpp"

#include <stdexcept>

#include <glm/ext/matrix_transform.hpp>

namespace lumin::scene {

    bool MeshHandle::isValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }

    glm::mat4 Transform::matrix() const {
        glm::mat4 result{1.0f};
        result = glm::translate(result, position);
        result = glm::rotate(result, glm::radians(rotationDegrees.x), glm::vec3{1.0f, 0.0f, 0.0f});
        result = glm::rotate(result, glm::radians(rotationDegrees.y), glm::vec3{0.0f, 1.0f, 0.0f});
        result = glm::rotate(result, glm::radians(rotationDegrees.z), glm::vec3{0.0f, 0.0f, 1.0f});
        return glm::scale(result, scale);
    }

    MeshHandle Level::addMesh(assets::Mesh mesh) {
        if (mesh.empty()) {
            throw std::invalid_argument("Level cannot add an empty mesh.");
        }

        const auto index = static_cast<std::uint32_t>(meshes_.size());
        meshes_.push_back(std::move(mesh));
        return MeshHandle{index};
    }

    std::uint32_t Level::addModel(MeshHandle meshHandle, Transform transform, Material material) {
        if (!meshHandle.isValid() || meshHandle.index >= meshes_.size()) {
            throw std::out_of_range("Level model references an invalid mesh handle.");
        }

        models_.push_back(ModelInstance{meshHandle, transform, material});
        return static_cast<std::uint32_t>(models_.size() - 1);
    }

    const assets::Mesh& Level::mesh(MeshHandle handle) const {
        if (!handle.isValid() || handle.index >= meshes_.size()) {
            throw std::out_of_range("Level mesh handle is invalid.");
        }
        return meshes_[handle.index];
    }

    const std::vector<assets::Mesh>& Level::meshes() const noexcept {
        return meshes_;
    }

    const std::vector<ModelInstance>& Level::models() const noexcept {
        return models_;
    }

}

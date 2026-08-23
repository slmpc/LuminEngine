#include "render/editor/ViewportPicking.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

namespace lumin::editor {
    namespace {
        bool intersectsBounds(const assets::Mesh& mesh, const glm::vec3& origin, const glm::vec3& direction) {
            glm::vec3 minimum{std::numeric_limits<float>::max()};
            glm::vec3 maximum{std::numeric_limits<float>::lowest()};
            for (const assets::Vertex& vertex : mesh.vertices) {
                minimum = glm::min(minimum, vertex.position);
                maximum = glm::max(maximum, vertex.position);
            }
            float nearDistance = 0.0f;
            float farDistance = std::numeric_limits<float>::max();
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(direction[axis]) < 1.0e-7f) {
                    if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
                        return false;
                    }
                    continue;
                }
                float first = (minimum[axis] - origin[axis]) / direction[axis];
                float second = (maximum[axis] - origin[axis]) / direction[axis];
                if (first > second) {
                    std::swap(first, second);
                }
                nearDistance = std::max(nearDistance, first);
                farDistance = std::min(farDistance, second);
                if (nearDistance > farDistance) {
                    return false;
                }
            }
            return farDistance >= 0.0f;
        }

        std::optional<float> intersectTriangle(const glm::vec3& origin, const glm::vec3& direction,
                                               const glm::vec3& first, const glm::vec3& second,
                                               const glm::vec3& third) {
            constexpr float epsilon = 1.0e-7f;
            const glm::vec3 edge1 = second - first;
            const glm::vec3 edge2 = third - first;
            const glm::vec3 perpendicular = glm::cross(direction, edge2);
            const float determinant = glm::dot(edge1, perpendicular);
            if (std::abs(determinant) < epsilon) {
                return std::nullopt;
            }
            const float inverse = 1.0f / determinant;
            const glm::vec3 offset = origin - first;
            const float u = glm::dot(offset, perpendicular) * inverse;
            if (u < 0.0f || u > 1.0f) {
                return std::nullopt;
            }
            const glm::vec3 cross = glm::cross(offset, edge1);
            const float v = glm::dot(direction, cross) * inverse;
            if (v < 0.0f || u + v > 1.0f) {
                return std::nullopt;
            }
            const float distance = glm::dot(edge2, cross) * inverse;
            return distance >= 0.0f ? std::optional<float>{distance} : std::nullopt;
        }
    } // namespace

    ViewportRay makeViewportRay(const scene::Camera& camera, float pixelX, float pixelY, float width, float height) {
        const float safeWidth = std::max(width, 1.0f);
        const float safeHeight = std::max(height, 1.0f);
        const float ndcX = pixelX / safeWidth * 2.0f - 1.0f;
        const float ndcY = 1.0f - pixelY / safeHeight * 2.0f;
        const glm::mat4 inverse = glm::inverse(camera.projectionMatrix(safeWidth / safeHeight) * camera.viewMatrix());
        glm::vec4 nearPoint = inverse * glm::vec4{ndcX, ndcY, -1.0f, 1.0f};
        glm::vec4 farPoint = inverse * glm::vec4{ndcX, ndcY, 1.0f, 1.0f};
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;
        return {camera.position(), glm::normalize(glm::vec3{farPoint - nearPoint})};
    }

    std::optional<ViewportPickResult> pickViewportModel(const scene::Level& level, const ViewportRay& ray) {
        std::optional<ViewportPickResult> closest;
        for (const scene::ModelHandle handle : level.modelHandles()) {
            const scene::ModelInstance& model = level.model(handle);
            const assets::Mesh& mesh = level.mesh(model.mesh);
            const glm::mat4 modelMatrix = model.transform.matrix();
            const glm::mat4 inverseModel = glm::inverse(modelMatrix);
            const glm::vec3 localOrigin = glm::vec3{inverseModel * glm::vec4{ray.origin, 1.0f}};
            const glm::vec3 localDirection = glm::normalize(glm::vec3{inverseModel * glm::vec4{ray.direction, 0.0f}});
            if (!intersectsBounds(mesh, localOrigin, localDirection)) {
                continue;
            }
            for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
                const auto localDistance = intersectTriangle(
                    localOrigin, localDirection, mesh.vertices[mesh.indices[index]].position,
                    mesh.vertices[mesh.indices[index + 1]].position, mesh.vertices[mesh.indices[index + 2]].position);
                if (!localDistance.has_value()) {
                    continue;
                }
                const glm::vec3 localHit = localOrigin + localDirection * *localDistance;
                const glm::vec3 worldHit = glm::vec3{modelMatrix * glm::vec4{localHit, 1.0f}};
                const float worldDistance = glm::length(worldHit - ray.origin);
                if (!closest.has_value() || worldDistance < closest->distance) {
                    closest = ViewportPickResult{handle, worldDistance, worldHit};
                }
            }
        }
        return closest;
    }

    std::optional<ViewportPickResult> pickViewportModel(const render::world::RenderWorldSnapshot& world,
                                                        const ViewportRay& ray) {
        std::optional<ViewportPickResult> closest;
        for (const render::world::RenderWorldInstance& instance : world.instances()) {
            if (instance.meshIndex >= world.meshes().size()) {
                continue;
            }
            const assets::Mesh& mesh = world.meshes()[instance.meshIndex].mesh;
            const glm::mat4 modelMatrix = instance.model.transform.matrix();
            const glm::mat4 inverseModel = glm::inverse(modelMatrix);
            const glm::vec3 localOrigin = glm::vec3{inverseModel * glm::vec4{ray.origin, 1.0f}};
            const glm::vec3 localDirection = glm::normalize(glm::vec3{inverseModel * glm::vec4{ray.direction, 0.0f}});
            if (!intersectsBounds(mesh, localOrigin, localDirection)) {
                continue;
            }
            for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
                const auto localDistance = intersectTriangle(
                    localOrigin, localDirection, mesh.vertices[mesh.indices[index]].position,
                    mesh.vertices[mesh.indices[index + 1]].position, mesh.vertices[mesh.indices[index + 2]].position);
                if (!localDistance.has_value()) {
                    continue;
                }
                const glm::vec3 localHit = localOrigin + localDirection * *localDistance;
                const glm::vec3 worldHit = glm::vec3{modelMatrix * glm::vec4{localHit, 1.0f}};
                const float worldDistance = glm::length(worldHit - ray.origin);
                if (!closest.has_value() || worldDistance < closest->distance) {
                    closest = ViewportPickResult{instance.modelHandle, worldDistance, worldHit};
                }
            }
        }
        return closest;
    }

} // namespace lumin::editor

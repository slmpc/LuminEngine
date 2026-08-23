#include "assets/ObjLoader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include <glm/geometric.hpp>
#include <tiny_obj_loader.h>

namespace lumin::assets {
    namespace {

        void generateMissingNormals(Mesh& mesh, const std::vector<int>& sourceVertexIndices,
                                    std::size_t sourceVertexCount) {
            for (auto& vertex : mesh.vertices) {
                vertex.normal = glm::vec3{0.0f};
            }

            std::vector<glm::vec3> sourceNormalSums(sourceVertexCount, glm::vec3{0.0f});
            std::vector<glm::vec3> fallbackNormalSums(mesh.vertices.size(), glm::vec3{0.0f});

            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const std::uint32_t aIndex = mesh.indices[i + 0];
                const std::uint32_t bIndex = mesh.indices[i + 1];
                const std::uint32_t cIndex = mesh.indices[i + 2];

                const Vertex& a = mesh.vertices[aIndex];
                const Vertex& b = mesh.vertices[bIndex];
                const Vertex& c = mesh.vertices[cIndex];

                const glm::vec3 ab = b.position - a.position;
                const glm::vec3 ac = c.position - a.position;
                const glm::vec3 normal = glm::cross(ab, ac);

                if (glm::length(normal) > 0.000001f) {
                    const glm::vec3 unitNormal = glm::normalize(normal);

                    const std::uint32_t triangleIndices[] = {aIndex, bIndex, cIndex};
                    for (const std::uint32_t vertexIndex : triangleIndices) {
                        fallbackNormalSums[vertexIndex] += unitNormal;

                        const int sourceIndex = sourceVertexIndices[vertexIndex];
                        if (sourceIndex >= 0 && static_cast<std::size_t>(sourceIndex) < sourceNormalSums.size()) {
                            sourceNormalSums[static_cast<std::size_t>(sourceIndex)] += unitNormal;
                        }
                    }
                }
            }

            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                const int sourceIndex = sourceVertexIndices[i];
                glm::vec3 normal = fallbackNormalSums[i];

                if (sourceIndex >= 0 && static_cast<std::size_t>(sourceIndex) < sourceNormalSums.size()) {
                    normal = sourceNormalSums[static_cast<std::size_t>(sourceIndex)];
                }

                if (glm::length(normal) > 0.000001f) {
                    mesh.vertices[i].normal = glm::normalize(normal);
                } else {
                    mesh.vertices[i].normal = glm::vec3{0.0f, 1.0f, 0.0f};
                }
            }
        }

        void generateMissingTexcoords(Mesh& mesh, const std::vector<bool>& missingTexcoordVertices) {
            if (mesh.vertices.empty()) {
                return;
            }

            glm::vec3 boundsMin{std::numeric_limits<float>::max()};
            glm::vec3 boundsMax{std::numeric_limits<float>::lowest()};
            for (const Vertex& vertex : mesh.vertices) {
                boundsMin = glm::min(boundsMin, vertex.position);
                boundsMax = glm::max(boundsMax, vertex.position);
            }

            const glm::vec3 boundsExtent = boundsMax - boundsMin;
            int axis = 0;
            if (boundsExtent.y > boundsExtent[axis]) {
                axis = 1;
            }
            if (boundsExtent.z > boundsExtent[axis]) {
                axis = 2;
            }

            const int radialAxisA = (axis + 1) % 3;
            const int radialAxisB = (axis + 2) % 3;
            const glm::vec3 boundsCenter = (boundsMin + boundsMax) * 0.5f;
            constexpr float epsilon = 0.000001f;
            constexpr float twoPi = 6.28318530717958647692f;
            const bool cylindricalProjection =
                boundsExtent[radialAxisA] > epsilon && boundsExtent[radialAxisB] > epsilon;

            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                if (!missingTexcoordVertices[i]) {
                    continue;
                }

                Vertex& vertex = mesh.vertices[i];
                float u = 0.0f;
                if (cylindricalProjection) {
                    const float radialA = vertex.position[radialAxisA] - boundsCenter[radialAxisA];
                    const float radialB = vertex.position[radialAxisB] - boundsCenter[radialAxisB];
                    const float angle = std::atan2(radialB, radialA);
                    u = (angle + 3.14159265358979323846f) / twoPi;
                } else {
                    const int planarUAxis =
                        boundsExtent[radialAxisA] > boundsExtent[radialAxisB] ? radialAxisA : radialAxisB;
                    const float planarURange = boundsExtent[planarUAxis];
                    if (planarURange > epsilon) {
                        u = (vertex.position[planarUAxis] - boundsMin[planarUAxis]) / planarURange;
                    }
                }

                const float axisRange = boundsExtent[axis];
                const float normalizedV =
                    axisRange > epsilon ? (vertex.position[axis] - boundsMin[axis]) / axisRange : 0.0f;
                vertex.texCoord = {u, 1.0f - normalizedV};
            }

            // Keep each triangle's U values close together when it crosses the cylindrical seam.
            if (!cylindricalProjection) {
                return;
            }

            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const std::uint32_t firstIndex = mesh.indices[i + 0];
                const std::uint32_t secondIndex = mesh.indices[i + 1];
                const std::uint32_t thirdIndex = mesh.indices[i + 2];
                if (!missingTexcoordVertices[firstIndex] || !missingTexcoordVertices[secondIndex] ||
                    !missingTexcoordVertices[thirdIndex]) {
                    continue;
                }

                Vertex& first = mesh.vertices[firstIndex];
                Vertex& second = mesh.vertices[secondIndex];
                Vertex& third = mesh.vertices[thirdIndex];

                const std::array<float, 3> values = {
                    first.texCoord.x,
                    second.texCoord.x,
                    third.texCoord.x,
                };
                std::array<int, 3> sortedIndices = {0, 1, 2};
                std::sort(sortedIndices.begin(), sortedIndices.end(), [&values](int lhs, int rhs) {
                    return values[lhs] < values[rhs];
                });

                float largestGap = 0.0f;
                int largestGapIndex = 0;
                for (int gapIndex = 0; gapIndex < 3; ++gapIndex) {
                    const float current = values[sortedIndices[gapIndex]];
                    const float next = values[sortedIndices[(gapIndex + 1) % 3]];
                    const float gap = gapIndex == 2 ? next + 1.0f - current : next - current;
                    if (gap > largestGap) {
                        largestGap = gap;
                        largestGapIndex = gapIndex;
                    }
                }

                if (largestGap > 0.5f) {
                    const int anchorIndex = sortedIndices[(largestGapIndex + 1) % 3];
                    const float anchor = values[anchorIndex];
                    const auto unwrap = [anchor](float value) {
                        while (value < anchor - 0.5f) {
                            value += 1.0f;
                        }
                        while (value > anchor + 0.5f) {
                            value -= 1.0f;
                        }
                        return value;
                    };
                    first.texCoord.x = unwrap(first.texCoord.x);
                    second.texCoord.x = unwrap(second.texCoord.x);
                    third.texCoord.x = unwrap(third.texCoord.x);
                }
            }
        }

        struct WorkingPart {
            ObjMeshPart part;
            std::vector<int> sourceVertexIndices;
            std::vector<bool> missingTexcoordVertices;
            bool missingNormals = false;
            bool missingTexcoords = false;
        };

        std::filesystem::path resolveTexturePath(const std::filesystem::path& objectPath, std::string textureName) {
            if (textureName.empty()) {
                return {};
            }
            // MTL exporters often write Windows separators even when the asset is consumed on another platform.
            std::ranges::replace(textureName, '\\', '/');
            const std::filesystem::path relative = std::filesystem::path{textureName}.lexically_normal();
            if (relative.empty() || relative.is_absolute()) {
                return {};
            }
            return (objectPath.parent_path() / relative).lexically_normal();
        }

        ObjMaterial importMaterial(const tinyobj::material_t& source, const std::filesystem::path& objectPath) {
            return ObjMaterial{
                .name = source.name,
                .diffuseColor = {source.diffuse[0], source.diffuse[1], source.diffuse[2]},
                .specularColor = {source.specular[0], source.specular[1], source.specular[2]},
                .shininess = std::max(source.shininess, 0.0f),
                // tinyobj 对缺失 Ni 使用 1.0；不透明旧材质回退到常见电介质 IOR，避免光追完全失去菲涅耳反射。
                .indexOfRefraction = std::isfinite(source.ior) && source.ior > 1.0f ? source.ior : 1.5f,
                .diffuseTexture = resolveTexturePath(objectPath, source.diffuse_texname),
                .normalTexture = resolveTexturePath(objectPath, source.normal_texname),
                .roughnessTexture = resolveTexturePath(objectPath, source.roughness_texname),
            };
        }

        Vertex readVertex(const tinyobj::attrib_t& attributes, const tinyobj::index_t& index, bool& missingNormal,
                          bool& missingTexcoord) {
            Vertex vertex;
            if (index.vertex_index < 0 ||
                static_cast<std::size_t>(3 * index.vertex_index + 2) >= attributes.vertices.size()) {
                throw std::runtime_error("OBJ face references an invalid position index.");
            }
            const std::size_t positionBase = static_cast<std::size_t>(3 * index.vertex_index);
            vertex.position = {
                attributes.vertices[positionBase + 0],
                attributes.vertices[positionBase + 1],
                attributes.vertices[positionBase + 2],
            };

            if (index.normal_index >= 0 &&
                static_cast<std::size_t>(3 * index.normal_index + 2) < attributes.normals.size()) {
                const std::size_t normalBase = static_cast<std::size_t>(3 * index.normal_index);
                vertex.normal = {
                    attributes.normals[normalBase + 0],
                    attributes.normals[normalBase + 1],
                    attributes.normals[normalBase + 2],
                };
            } else {
                missingNormal = true;
            }

            if (index.texcoord_index >= 0 &&
                static_cast<std::size_t>(2 * index.texcoord_index + 1) < attributes.texcoords.size()) {
                const std::size_t texcoordBase = static_cast<std::size_t>(2 * index.texcoord_index);
                vertex.texCoord = {
                    attributes.texcoords[texcoordBase + 0],
                    1.0f - attributes.texcoords[texcoordBase + 1],
                };
            } else {
                missingTexcoord = true;
            }
            return vertex;
        }

        void appendTriangle(WorkingPart& destination, const tinyobj::attrib_t& attributes,
                            const tinyobj::mesh_t& sourceMesh, std::size_t indexOffset) {
            for (std::size_t vertexInFace = 0; vertexInFace < 3; ++vertexInFace) {
                const tinyobj::index_t index = sourceMesh.indices[indexOffset + vertexInFace];
                bool missingNormal = false;
                bool missingTexcoord = false;
                Vertex vertex = readVertex(attributes, index, missingNormal, missingTexcoord);

                destination.part.mesh.indices.push_back(
                    static_cast<std::uint32_t>(destination.part.mesh.vertices.size()));
                destination.sourceVertexIndices.push_back(index.vertex_index);
                destination.missingTexcoordVertices.push_back(missingTexcoord);
                destination.part.mesh.vertices.push_back(std::move(vertex));
                destination.missingNormals |= missingNormal;
                destination.missingTexcoords |= missingTexcoord;
            }
        }

        void finalizePart(WorkingPart& working, std::size_t sourceVertexCount) {
            if (working.missingNormals) {
                generateMissingNormals(working.part.mesh, working.sourceVertexIndices, sourceVertexCount);
            }
            if (working.missingTexcoords) {
                generateMissingTexcoords(working.part.mesh, working.missingTexcoordVertices);
            }
        }

    } // namespace

    bool Mesh::empty() const noexcept {
        return vertices.empty() || indices.empty();
    }

    bool ObjModel::empty() const noexcept {
        return parts.empty();
    }

    ObjModel ObjLoader::loadModel(const std::filesystem::path& path) {
        tinyobj::ObjReaderConfig config;
        config.triangulate = true;

        if (path.has_parent_path()) {
            config.mtl_search_path = path.parent_path().string();
        }

        tinyobj::ObjReader reader;
        if (!reader.ParseFromFile(path.string(), config)) {
            std::string message = "Failed to load OBJ file: " + path.string();
            if (!reader.Error().empty()) {
                message += "\n" + reader.Error();
            }
            throw std::runtime_error(message);
        }

        if (!reader.Warning().empty()) {
            // Warnings are kept available through tinyobj, but loading can proceed.
        }

        const tinyobj::attrib_t& attributes = reader.GetAttrib();
        const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
        const std::vector<tinyobj::material_t>& materials = reader.GetMaterials();

        ObjModel model;
        model.name = path.stem().string();
        std::vector<WorkingPart> workingParts;
        std::unordered_map<int, std::size_t> partByMaterial;

        for (const tinyobj::shape_t& shape : shapes) {
            std::size_t indexOffset = 0;

            for (std::size_t face = 0; face < shape.mesh.num_face_vertices.size(); ++face) {
                const int vertexCount = shape.mesh.num_face_vertices[face];

                if (vertexCount != 3) {
                    indexOffset += static_cast<std::size_t>(vertexCount);
                    continue;
                }

                const int materialId = face < shape.mesh.material_ids.size() ? shape.mesh.material_ids[face] : -1;
                auto [partIterator, inserted] = partByMaterial.try_emplace(materialId, workingParts.size());
                if (inserted) {
                    ObjMaterial material;
                    if (materialId >= 0 && static_cast<std::size_t>(materialId) < materials.size()) {
                        material = importMaterial(materials[static_cast<std::size_t>(materialId)], path);
                    }
                    if (material.name.empty()) {
                        material.name = materialId < 0 ? "default" : "material_" + std::to_string(materialId);
                    }
                    WorkingPart working;
                    working.part.name = material.name;
                    working.part.mesh.name = model.name + "/" + material.name;
                    working.part.material = std::move(material);
                    workingParts.push_back(std::move(working));
                }
                appendTriangle(workingParts[partIterator->second], attributes, shape.mesh, indexOffset);
                indexOffset += static_cast<std::size_t>(vertexCount);
            }
        }

        for (WorkingPart& working : workingParts) {
            if (working.part.mesh.empty()) {
                continue;
            }
            finalizePart(working, attributes.vertices.size() / 3);
            model.parts.push_back(std::move(working.part));
        }

        if (model.empty()) {
            throw std::runtime_error("OBJ file did not contain any triangle geometry: " + path.string());
        }

        return model;
    }

    Mesh ObjLoader::load(const std::filesystem::path& path) {
        ObjModel model = loadModel(path);
        Mesh mesh;
        mesh.name = model.name;
        for (ObjMeshPart& part : model.parts) {
            if (part.mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() - mesh.vertices.size()) {
                throw std::length_error("OBJ contains too many vertices for 32-bit indices: " + path.string());
            }
            const std::uint32_t vertexOffset = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.insert(mesh.vertices.end(), std::make_move_iterator(part.mesh.vertices.begin()),
                                 std::make_move_iterator(part.mesh.vertices.end()));
            mesh.indices.reserve(mesh.indices.size() + part.mesh.indices.size());
            for (const std::uint32_t index : part.mesh.indices) {
                mesh.indices.push_back(vertexOffset + index);
            }
        }
        return mesh;
    }

} // namespace lumin::assets

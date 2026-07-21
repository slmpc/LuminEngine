#include "lumin/assets/ObjLoader.hpp"

#include <cstddef>
#include <stdexcept>

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

    } // namespace

    bool Mesh::empty() const noexcept {
        return vertices.empty() || indices.empty();
    }

    Mesh ObjLoader::load(const std::filesystem::path& path) {
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

        Mesh mesh;
        mesh.name = path.stem().string();

        bool missingNormals = false;
        std::vector<int> sourceVertexIndices;

        for (const tinyobj::shape_t& shape : shapes) {
            std::size_t indexOffset = 0;

            for (std::size_t face = 0; face < shape.mesh.num_face_vertices.size(); ++face) {
                const int vertexCount = shape.mesh.num_face_vertices[face];

                if (vertexCount != 3) {
                    indexOffset += static_cast<std::size_t>(vertexCount);
                    continue;
                }

                for (int vertexInFace = 0; vertexInFace < 3; ++vertexInFace) {
                    const tinyobj::index_t index =
                        shape.mesh.indices[indexOffset + static_cast<std::size_t>(vertexInFace)];

                    Vertex vertex;
                    if (index.vertex_index >= 0) {
                        const std::size_t base = static_cast<std::size_t>(3 * index.vertex_index);
                        vertex.position = {
                            attributes.vertices[base + 0],
                            attributes.vertices[base + 1],
                            attributes.vertices[base + 2],
                        };
                    }

                    if (index.normal_index >= 0) {
                        const std::size_t base = static_cast<std::size_t>(3 * index.normal_index);
                        vertex.normal = {
                            attributes.normals[base + 0],
                            attributes.normals[base + 1],
                            attributes.normals[base + 2],
                        };
                    } else {
                        missingNormals = true;
                    }

                    if (index.texcoord_index >= 0) {
                        const std::size_t base = static_cast<std::size_t>(2 * index.texcoord_index);
                        vertex.texCoord = {
                            attributes.texcoords[base + 0],
                            1.0f - attributes.texcoords[base + 1],
                        };
                    }

                    mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                    sourceVertexIndices.push_back(index.vertex_index);
                    mesh.vertices.push_back(vertex);
                }

                indexOffset += 3;
            }
        }

        if (mesh.empty()) {
            throw std::runtime_error("OBJ file did not contain any triangle geometry: " + path.string());
        }

        if (missingNormals) {
            generateMissingNormals(mesh, sourceVertexIndices, attributes.vertices.size() / 3);
        }

        return mesh;
    }

} // namespace lumin::assets

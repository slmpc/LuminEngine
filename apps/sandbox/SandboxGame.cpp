#include "SandboxGame.hpp"

#include "lumin/assets/ObjLoader.hpp"
#include "lumin/scene/Terrain.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>

namespace lumin::sandbox {
    namespace {

        assets::Mesh createFallbackCube() {
            assets::Mesh mesh;
            mesh.name = "fallback-cube";
            mesh.vertices = {
                {{-1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
                {{1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
                {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
                {{-1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
                {{1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
                {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
                {{-1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
                {{1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
                {{-1.0f, -1.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
                {{-1.0f, -1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
                {{-1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
                {{-1.0f, 1.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
                {{1.0f, -1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
                {{1.0f, -1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
                {{1.0f, 1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
                {{1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
                {{-1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
                {{1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
                {{1.0f, 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
                {{-1.0f, 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
                {{-1.0f, -1.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
                {{1.0f, -1.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
                {{1.0f, -1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
                {{-1.0f, -1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
            };
            mesh.indices = {
                0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
                12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20,
            };
            return mesh;
        }

        assets::Mesh createFallbackPyramid() {
            assets::Mesh mesh;
            mesh.name = "fallback-pyramid";
            const glm::vec3 top{0.0f, 1.2f, 0.0f};
            const glm::vec3 a{-1.0f, -1.0f, 1.0f};
            const glm::vec3 b{1.0f, -1.0f, 1.0f};
            const glm::vec3 c{1.0f, -1.0f, -1.0f};
            const glm::vec3 d{-1.0f, -1.0f, -1.0f};

            auto appendTriangle = [&mesh](const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2) {
                const glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
                const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back({p0, normal, {0.0f, 0.0f}});
                mesh.vertices.push_back({p1, normal, {1.0f, 0.0f}});
                mesh.vertices.push_back({p2, normal, {0.5f, 1.0f}});
                mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2});
            };
            appendTriangle(a, b, top);
            appendTriangle(b, c, top);
            appendTriangle(c, d, top);
            appendTriangle(d, a, top);
            appendTriangle(d, c, b);
            appendTriangle(b, a, d);
            return mesh;
        }

        scene::Transform fitTransform(const assets::Mesh& mesh, const glm::vec3& targetPosition) {
            glm::vec3 minBounds{std::numeric_limits<float>::max()};
            glm::vec3 maxBounds{std::numeric_limits<float>::lowest()};
            for (const assets::Vertex& vertex : mesh.vertices) {
                minBounds = glm::min(minBounds, vertex.position);
                maxBounds = glm::max(maxBounds, vertex.position);
            }
            const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
            float radius = 0.0f;
            for (const assets::Vertex& vertex : mesh.vertices) {
                radius = std::max(radius, glm::length(vertex.position - center));
            }
            const float scale = radius > 0.0001f ? 1.35f / radius : 1.0f;
            scene::Transform transform;
            transform.scale = glm::vec3{scale};
            transform.position = targetPosition - center * scale;
            return transform;
        }

        std::optional<std::filesystem::path> defaultObjPath() {
#if defined(LUMIN_ASSET_DIR)
            const std::filesystem::path path = std::filesystem::path{LUMIN_ASSET_DIR} / "models" / "stanford-bunny.obj";
#else
            const std::filesystem::path path = std::filesystem::path{"assets"} / "models" / "stanford-bunny.obj";
#endif
            if (std::filesystem::exists(path)) {
                return path;
            }
            return std::nullopt;
        }

        scene::PbrTextureSet defaultAsphaltTextures() {
#if defined(LUMIN_ASSET_DIR)
            const std::filesystem::path directory =
                std::filesystem::path{LUMIN_ASSET_DIR} / "materials" / "aerial_asphalt_01";
#else
            const std::filesystem::path directory = std::filesystem::path{"assets"} / "materials" / "aerial_asphalt_01";
#endif
            return scene::PbrTextureSet{
                .baseColor = directory / "aerial_asphalt_01_diff_1k.jpg",
                .normal = directory / "aerial_asphalt_01_nor_gl_1k.png",
                .roughness = directory / "aerial_asphalt_01_rough_1k.jpg",
                .flipNormalY = true,
            };
        }

    } // namespace

    SandboxGame::SandboxGame(SandboxGameConfig config) : config_(std::move(config)) {
    }

    void SandboxGame::initialize(game::GameContext& context) {
        assets::Mesh primaryMesh;
        if (config_.objPath.has_value()) {
            primaryMesh = assets::ObjLoader::load(*config_.objPath);
            std::cout << "Loaded OBJ '" << primaryMesh.name << "' with " << primaryMesh.vertices.size()
                      << " vertices and " << primaryMesh.indices.size() / 3 << " triangles.\n";
        } else if (const auto defaultPath = defaultObjPath(); defaultPath.has_value()) {
            primaryMesh = assets::ObjLoader::load(*defaultPath);
            std::cout << "Loaded default OBJ '" << primaryMesh.name << "' with " << primaryMesh.vertices.size()
                      << " vertices and " << primaryMesh.indices.size() / 3 << " triangles.\n";
        } else {
            primaryMesh = createFallbackCube();
            std::cout << "No OBJ file supplied. Using the built-in cube.\n";
        }

        assets::Mesh secondaryMesh = createFallbackPyramid();
        const scene::Transform leftTransform = fitTransform(primaryMesh, {-2.2f, 0.0f, 0.0f});
        const scene::Transform rightTransform = fitTransform(primaryMesh, {2.2f, 0.0f, 0.0f});
        const scene::Transform centerTransform = fitTransform(secondaryMesh, {0.0f, 0.0f, 0.0f});
        const scene::MeshHandle primary = context.level.addMesh(std::move(primaryMesh));
        const scene::MeshHandle secondary = context.level.addMesh(std::move(secondaryMesh));

        scene::Material asphalt;
        asphalt.albedo = {1.0f, 1.0f, 1.0f};
        asphalt.roughness = 1.0f;
        asphalt.metallic = 0.0f;
        asphalt.textureScale = 2.5f;
        asphalt.textures = defaultAsphaltTextures();
        scene::Material green;
        green.albedo = {0.25f, 0.76f, 0.46f};
        green.roughness = 0.58f;

        context.level.addModel(primary, leftTransform, asphalt);
        context.level.addModel(secondary, centerTransform, green);
        context.level.addModel(primary, rightTransform, asphalt);
        scene::TerrainDesc terrainDescription;
        terrainDescription.resolutionX = 48;
        terrainDescription.resolutionZ = 48;
        terrainDescription.sizeX = 18.0f;
        terrainDescription.sizeZ = 18.0f;
        scene::Material terrainMaterial;
        terrainMaterial.albedo = {0.18f, 0.42f, 0.23f};
        terrainMaterial.roughness = 0.82f;
        const scene::ActorHandle terrainActor =
            context.level.spawnActor<scene::TerrainActor>(terrainDescription, terrainMaterial);
        if (scene::Actor* actor = context.level.actor(terrainActor); actor != nullptr) {
            actor->setTransform(scene::Transform{.position = {0.0f, -1.35f, 0.0f}});
        }
        std::cout << "Level assembled with " << context.level.meshes().size() << " meshes and "
                  << context.level.models().size() << " model instances and " << context.level.actorCount()
                  << " actors.\n";
    }

    void SandboxGame::tick(game::GameContext&, float) {
    }

} // namespace lumin::sandbox

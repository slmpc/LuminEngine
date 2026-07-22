#include "lumin/core/Application.hpp"

#include "lumin/scene/CameraController.hpp"
#include "lumin/scene/Terrain.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>

namespace lumin::core {
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

        platform::RenderDocAttachment attachRenderDoc(const ApplicationConfig& config) {
            if (!config.enableRenderDoc) {
                return {};
            }
            if (!config.renderDocPath.has_value()) {
                throw std::invalid_argument("RenderDoc is enabled, but no library path was supplied");
            }

            platform::RenderDocAttachment attachment(*config.renderDocPath);
            std::cout << "RenderDoc attached from '" << config.renderDocPath->string() << "'.\n";
            return attachment;
        }

    } // namespace

    Application::Application(ApplicationConfig config)
        : config_(std::move(config)), renderDoc_(attachRenderDoc(config_)),
          window_(platform::WindowDesc{config_.width, config_.height, config_.title}),
          vulkan_(window_, render::VulkanContextDesc{config_.title,
#if defined(LUMIN_ENABLE_VALIDATION)
                                                     true
#else
                                                     false
#endif
                           }) {
    }

    Application::~Application() = default;

    int Application::run() {
        loadScene();

        const std::filesystem::path shaderDirectory =
#if defined(LUMIN_SHADER_DIR)
            LUMIN_SHADER_DIR;
#else
            "shaders";
#endif
        renderer_ = std::make_unique<render::LevelRenderer>(window_, vulkan_, level_, shaderDirectory);
        std::cout << "Level renderer ready: models=" << renderer_->modelCount()
                  << " mdiDraws=" << renderer_->mdiDrawCount()
                  << " gbuffer=position+normalRoughness+albedoMetallic+motion csm=4 ssao=on taa=on\n";

        auto previousTime = std::chrono::steady_clock::now();
        while (!window_.shouldClose()) {
            window_.pollEvents();
            if (window_.isKeyDown(platform::Key::Escape)) {
                break;
            }

            const auto currentTime = std::chrono::steady_clock::now();
            const float deltaSeconds =
                std::clamp(std::chrono::duration<float>(currentTime - previousTime).count(), 0.0f, 0.1f);
            previousTime = currentTime;

            scene::CameraInput input;
            input.forward = static_cast<float>(window_.isKeyDown(platform::Key::W)) -
                            static_cast<float>(window_.isKeyDown(platform::Key::S));
            input.right = static_cast<float>(window_.isKeyDown(platform::Key::D)) -
                          static_cast<float>(window_.isKeyDown(platform::Key::A));
            input.up = static_cast<float>(window_.isKeyDown(platform::Key::Space)) -
                       static_cast<float>(window_.isKeyDown(platform::Key::LeftControl));
            scene::CameraController::update(camera_, input, deltaSeconds);
            level_.tick(deltaSeconds);
            renderer_->drawFrame(camera_, renderSettings_);
        }

        renderer_->waitIdle();
        return 0;
    }

    void Application::loadScene() {
        assets::Mesh primaryMesh;
        if (config_.objPath.has_value()) {
            primaryMesh = assets::ObjLoader::load(*config_.objPath);
            std::cout << "Loaded OBJ '" << primaryMesh.name << "' with " << primaryMesh.vertices.size()
                      << " vertices and " << primaryMesh.indices.size() / 3 << " triangles.\n";
        } else {
            if (const auto defaultPath = defaultObjPath(); defaultPath.has_value()) {
                primaryMesh = assets::ObjLoader::load(*defaultPath);
                std::cout << "Loaded default OBJ '" << primaryMesh.name << "' with " << primaryMesh.vertices.size()
                          << " vertices and " << primaryMesh.indices.size() / 3 << " triangles.\n";
            } else {
                primaryMesh = createFallbackCube();
                std::cout << "No OBJ file supplied. Using the built-in cube.\n";
            }
        }

        assets::Mesh secondaryMesh = createFallbackPyramid();
        const scene::Transform leftTransform = fitTransform(primaryMesh, {-2.2f, 0.0f, 0.0f});
        const scene::Transform rightTransform = fitTransform(primaryMesh, {2.2f, 0.0f, 0.0f});
        const scene::Transform centerTransform = fitTransform(secondaryMesh, {0.0f, 0.0f, 0.0f});
        const scene::MeshHandle primary = level_.addMesh(std::move(primaryMesh));
        const scene::MeshHandle secondary = level_.addMesh(std::move(secondaryMesh));

        scene::Material asphalt;
        asphalt.albedo = {1.0f, 1.0f, 1.0f};
        asphalt.roughness = 1.0f;
        asphalt.metallic = 0.0f;
        asphalt.textureScale = 2.5f;
        asphalt.textures = defaultAsphaltTextures();
        scene::Material green;
        green.albedo = {0.25f, 0.76f, 0.46f};
        green.roughness = 0.58f;

        level_.addModel(primary, leftTransform, asphalt);
        level_.addModel(secondary, centerTransform, green);
        level_.addModel(primary, rightTransform, asphalt);
        scene::TerrainDesc terrainDescription;
        terrainDescription.resolutionX = 48;
        terrainDescription.resolutionZ = 48;
        terrainDescription.sizeX = 18.0f;
        terrainDescription.sizeZ = 18.0f;
        scene::Material terrainMaterial;
        terrainMaterial.albedo = {0.18f, 0.42f, 0.23f};
        terrainMaterial.roughness = 0.82f;
        const scene::ActorHandle terrainActor =
            level_.spawnActor<scene::TerrainActor>(terrainDescription, terrainMaterial);
        if (scene::Actor* actor = level_.actor(terrainActor); actor != nullptr) {
            actor->setTransform(scene::Transform{.position = {0.0f, -1.35f, 0.0f}});
        }
        std::cout << "Level assembled with " << level_.meshes().size() << " meshes and " << level_.models().size()
                  << " model instances and " << level_.actorCount() << " actors.\n";
    }

} // namespace lumin::core

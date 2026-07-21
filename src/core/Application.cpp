#include "lumin/core/Application.hpp"

#include <iostream>
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

        render::FrameGraphTextureDesc makeBackBufferDesc(VkExtent2D extent) {
            render::FrameGraphTextureDesc desc;
            desc.width = extent.width;
            desc.height = extent.height;
            desc.format = VK_FORMAT_B8G8R8A8_UNORM;
            desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            return desc;
        }

        render::FrameGraphTextureDesc makeDepthDesc(VkExtent2D extent) {
            render::FrameGraphTextureDesc desc;
            desc.width = extent.width;
            desc.height = extent.height;
            desc.format = VK_FORMAT_D32_SFLOAT;
            desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            return desc;
        }

    } // namespace

    Application::Application(ApplicationConfig config)
        : config_(std::move(config)), window_(platform::WindowDesc{config_.width, config_.height, config_.title}),
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
        buildFrameGraph();

        const std::filesystem::path shaderDirectory =
#if defined(LUMIN_SHADER_DIR)
            LUMIN_SHADER_DIR;
#else
            "shaders";
#endif
        render::ObjRenderer renderer(window_, vulkan_, *mesh_, shaderDirectory);

        std::uint32_t frameIndex = 0;
        while (!window_.shouldClose()) {
            window_.pollEvents();

            render::FrameGraphContext context;
            context.device = vulkan_.device();
            context.frameIndex = frameIndex;
            context.log = frameIndex == 0 ? &std::cout : nullptr;
            frameGraph_.execute(context);
            renderer.drawFrame(renderSettings_);

            ++frameIndex;
        }

        renderer.waitIdle();
        return 0;
    }

    void Application::loadScene() {
        if (!config_.objPath.has_value()) {
            if (const auto defaultPath = defaultObjPath(); defaultPath.has_value()) {
                mesh_ = assets::ObjLoader::load(*defaultPath);
                std::cout << "Loaded default OBJ '" << mesh_->name << "' with " << mesh_->vertices.size()
                          << " vertices and " << mesh_->indices.size() / 3 << " triangles.\n";
                return;
            }

            mesh_ = createFallbackCube();
            std::cout << "No OBJ file supplied. Rendering the built-in cube.\n";
            return;
        }

        mesh_ = assets::ObjLoader::load(*config_.objPath);
        std::cout << "Loaded OBJ '" << mesh_->name << "' with " << mesh_->vertices.size() << " vertices and "
                  << mesh_->indices.size() / 3 << " triangles.\n";
    }

    void Application::buildFrameGraph() {
        frameGraph_.reset();

        const VkExtent2D extent = window_.framebufferExtent();
        const auto backBuffer = frameGraph_.importTexture("swapchain.backbuffer", makeBackBufferDesc(extent));
        const auto depth = frameGraph_.createTexture("scene.depth", makeDepthDesc(extent));

        render::FrameGraphBufferDesc geometryDesc;
        geometryDesc.size =
            mesh_.has_value() ? static_cast<std::uint64_t>(mesh_->vertices.size() * sizeof(assets::Vertex)) : 1;
        geometryDesc.usage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const auto geometry = frameGraph_.importBuffer("scene.geometry", geometryDesc);

        frameGraph_.addPass(
            "Upload geometry", render::FrameGraphPassType::Transfer,
            [geometry](render::FrameGraphBuilder& builder) {
                builder.write(geometry, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            },
            [](const render::FrameGraphContext& context) {
                if (context.log != nullptr) {
                    *context.log << "  upload geometry resources\n";
                }
            });

        frameGraph_.addPass(
            "Depth prepass", render::FrameGraphPassType::Graphics,
            [geometry, depth](render::FrameGraphBuilder& builder) {
                builder.read(geometry, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
                builder.write(depth, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            },
            [](const render::FrameGraphContext& context) {
                if (context.log != nullptr) {
                    *context.log << "  record depth prepass\n";
                }
            });

        frameGraph_.addPass(
            "Opaque color", render::FrameGraphPassType::Graphics,
            [geometry, depth, backBuffer](render::FrameGraphBuilder& builder) {
                builder.read(geometry, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
                builder.read(depth, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
                builder.write(backBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [](const render::FrameGraphContext& context) {
                if (context.log != nullptr) {
                    *context.log << "  record opaque color pass\n";
                }
            });

        frameGraph_.addPass(
            "Present", render::FrameGraphPassType::Present,
            [backBuffer](render::FrameGraphBuilder& builder) {
                builder.read(backBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT);
            },
            [](const render::FrameGraphContext& context) {
                if (context.log != nullptr) {
                    *context.log << "  present frame\n";
                }
            });

        frameGraph_.compile();
    }

} // namespace lumin::core

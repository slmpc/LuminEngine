#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef LUMIN_TEST_SOURCE_DIR
#define LUMIN_TEST_SOURCE_DIR "."
#endif

namespace {

    namespace fs = std::filesystem;

    [[nodiscard]] std::string readSource(const fs::path& root, const fs::path& relative) {
        std::ifstream input(root / relative, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot read architecture input: " + relative.generic_string());
        }
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    void require(bool condition, std::string message) {
        if (!condition) {
            throw std::runtime_error(std::move(message));
        }
    }

    [[nodiscard]] std::string_view targetBlock(const std::string& cmake, std::string_view target,
                                                std::string_view nextTarget) {
        const std::size_t begin = cmake.find("add_library(" + std::string{target});
        const std::size_t end = cmake.find("add_library(" + std::string{nextTarget}, begin + 1);
        require(begin != std::string::npos && end != std::string::npos && begin < end,
                "Cannot locate CMake target block for " + std::string{target});
        return std::string_view{cmake}.substr(begin, end - begin);
    }

    void verifyRealBuildTargets(const fs::path& root) {
        const std::string cmake = readSource(root, "render/CMakeLists.txt");
        for (const auto& [implementation, alias] :
             std::vector<std::pair<std::string_view, std::string_view>>{
                 {"lumin_render_core", "Lumin::RenderCore"},
                 {"lumin_render_rhi", "Lumin::RenderRhi"},
                 {"lumin_vulkan_backend", "Lumin::VulkanBackend"},
                 {"lumin_render_runtime", "Lumin::RenderRuntime"},
                 {"lumin_editor_module", "Lumin::Editor"},
             }) {
            require(cmake.find("add_library(" + std::string{implementation} + " STATIC") != std::string::npos,
                    "Architecture target is not a real static library: " + std::string{implementation});
            require(cmake.find("add_library(" + std::string{alias} + " ALIAS " + std::string{implementation} + ")") !=
                        std::string::npos,
                    "Architecture alias does not name its own build product: " + std::string{alias});
        }

        const std::string_view runtime = targetBlock(cmake, "lumin_render_runtime", "lumin_editor_module");
        for (std::string_view forbidden :
             {"Lumin::RenderFeatures", "Lumin::RenderPipelines", "DefaultRender", "level/", "features/", "gi/"}) {
            require(runtime.find(forbidden) == std::string_view::npos,
                    "RenderRuntime depends on a concrete pipeline token: " + std::string{forbidden});
        }
    }

    void verifyRuntimeSourceBoundary(const fs::path& root) {
        const std::string renderer = readSource(root, "render/runtime/Renderer.cpp");
        for (std::string_view forbidden : {"render/features/", "render/pipelines/", "render/gi/", "render/atmosphere/",
                                           "render/presentation/", "LevelRenderer", "ObjRenderer", "ImGui::", "SDL_",
                                           "scene::Level", "scene::Camera"}) {
            require(renderer.find(forbidden) == std::string::npos,
                    "Renderer runtime crossed a concrete/main-thread boundary: " + std::string{forbidden});
        }
        require(renderer.find("IRenderPipelineSession") != std::string::npos &&
                    renderer.find("IRenderPipelineSessionFactory") != std::string::npos,
                "Renderer runtime must depend only on injected pipeline session contracts.");
    }

    void verifyObsoleteArchitectureRemoved(const fs::path& root) {
        std::string renderSources;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root / "render")) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string extension = entry.path().extension().string();
            if (extension == ".hpp" || extension == ".cpp") {
                renderSources += readSource(root, fs::relative(entry.path(), root));
            }
        }
        for (std::string_view forbidden : {"LevelRenderFeatureKind", "LevelRenderFrameData", "TextureManager",
                                           "PipelineManager", "LevelRenderer::Impl", "class LevelRenderer",
                                           "class ObjRenderer"}) {
            require(renderSources.find(forbidden) == std::string::npos,
                    "Obsolete render architecture token remains: " + std::string{forbidden});
        }
    }

} // namespace

int main() {
    try {
        const fs::path root = fs::path{LUMIN_TEST_SOURCE_DIR};
        verifyRealBuildTargets(root);
        verifyRuntimeSourceBoundary(root);
        verifyObsoleteArchitectureRemoved(root);
        std::cout << "Render module boundaries passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Render module boundary test failed: " << exception.what() << '\n';
        return 1;
    }
}

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace lumin::assets {

    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        glm::vec2 texCoord{0.0f};
    };

    struct Mesh {
        std::string name;
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;

        [[nodiscard]] bool empty() const noexcept;
    };

    class ObjLoader {
    public:
        [[nodiscard]] static Mesh load(const std::filesystem::path& path);
    };

} // namespace lumin::assets

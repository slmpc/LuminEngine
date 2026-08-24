#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace lumin::assets {

    /** OBJ 顶点数据。 */
    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        glm::vec2 texCoord{0.0f};
    };

    /** 可由渲染场景持有的单一三角形网格。 */
    struct Mesh {
        std::string name;
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;

        /** 返回网格是否缺少可绘制的顶点或索引。 */
        [[nodiscard]] bool empty() const noexcept;
    };

    /**
     * OBJ/MTL 中可映射到引擎材质的元数据。
     *
     * 贴图路径解析为相对于 OBJ 所在目录的绝对路径；未声明的贴图保持为空，由渲染器使用对应 fallback。
     */
    struct ObjMaterial {
        std::string name;
        glm::vec3 diffuseColor{1.0f};
        glm::vec3 specularColor{0.04f};
        float shininess = 48.0f;
        /// MTL `Ni` 声明的介质折射率；缺失时使用常见电介质默认值 1.5。
        float indexOfRefraction = 1.5f;
        std::filesystem::path diffuseTexture;
        std::filesystem::path normalTexture;
        std::filesystem::path roughnessTexture;
    };

    /** 按一个 OBJ 材质聚合的可独立绘制网格分区。 */
    struct ObjMeshPart {
        std::string name;
        Mesh mesh;
        ObjMaterial material;
    };

    /** OBJ 文件中的全部材质分区；分区顺序按文件中首次出现的顺序稳定生成。 */
    struct ObjModel {
        std::string name;
        std::vector<ObjMeshPart> parts;

        /** 返回模型是否不包含任何可绘制分区。 */
        [[nodiscard]] bool empty() const noexcept;
    };

    /** 加载 Wavefront OBJ 几何及其 MTL 材质声明。 */
    class ObjLoader {
    public:
        /**
         * 加载并合并 OBJ 中的所有材质分区。
         *
         * 此接口保留给只支持单一材质的调用方；需要保留 MTL 材质时应使用 `loadModel`。
         */
        [[nodiscard]] static Mesh load(const std::filesystem::path& path);

        /** 加载 OBJ，并按 `usemtl` 材质聚合为多个可独立绘制的网格分区。 */
        [[nodiscard]] static ObjModel loadModel(const std::filesystem::path& path);
    };

} // namespace lumin::assets

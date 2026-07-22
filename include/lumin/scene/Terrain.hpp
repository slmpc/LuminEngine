#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "lumin/scene/Level.hpp"

namespace lumin::scene {

    struct TerrainDesc {
        // Resolution values describe the number of quads. A value of N
        // produces (N + 1) samples along that axis.
        std::uint32_t resolutionX = 32;
        std::uint32_t resolutionZ = 32;
        std::uint32_t resolution = 0;
        std::uint32_t segmentsX = 0;
        std::uint32_t segmentsZ = 0;

        // width/depth are aliases for sizeX/sizeZ when supplied (> 0).
        float sizeX = 20.0f;
        float sizeZ = 20.0f;
        float width = 0.0f;
        float depth = 0.0f;
        float heightScale = 1.0f;
        float amplitude = 1.0f;

        std::function<float(float, float)> heightFunction;
    };

    using TerrainSettings = TerrainDesc;
    using TerrainHeightFunction = std::function<float(float, float)>;

    class Terrain {
    public:
        explicit Terrain(TerrainDesc description = {});
        Terrain(std::uint32_t resolutionX, std::uint32_t resolutionZ, float sizeX = 20.0f, float sizeZ = 20.0f,
                TerrainHeightFunction heightFunction = {});

        [[nodiscard]] static Terrain generate(TerrainDesc description = {});
        [[nodiscard]] static assets::Mesh generateMesh(const TerrainDesc& description);

        void rebuild();
        void setHeightFunction(TerrainHeightFunction heightFunction);
        void setHeightSample(std::uint32_t x, std::uint32_t z, float height);

        [[nodiscard]] float heightAt(float x, float z) const noexcept;
        [[nodiscard]] float sampleHeight(float x, float z) const noexcept;
        [[nodiscard]] const TerrainDesc& description() const noexcept;
        [[nodiscard]] const assets::Mesh& mesh() const noexcept;
        [[nodiscard]] std::uint32_t resolutionX() const noexcept;
        [[nodiscard]] std::uint32_t resolutionZ() const noexcept;
        [[nodiscard]] float sizeX() const noexcept;
        [[nodiscard]] float sizeZ() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] std::uint64_t meshRevision() const noexcept;

    private:
        void rebuildMesh();

        TerrainDesc description_;
        std::uint32_t resolutionX_ = 1;
        std::uint32_t resolutionZ_ = 1;
        float sizeX_ = 1.0f;
        float sizeZ_ = 1.0f;
        std::vector<float> heights_;
        assets::Mesh mesh_;
        std::uint64_t revision_ = 0;
    };

    class TerrainActor final : public Actor {
    public:
        explicit TerrainActor(TerrainDesc description = {}, Material material = {});
        explicit TerrainActor(Terrain terrain, Material material = {});

        void onSpawn(Level& level) override;
        void onDestroy(Level& level) override;
        void onTick(float deltaSeconds) override;

        [[nodiscard]] Terrain& terrain() noexcept;
        [[nodiscard]] const Terrain& terrain() const noexcept;
        void setTerrain(TerrainDesc description);
        void setTerrain(Terrain terrain);
        [[nodiscard]] float heightAt(float x, float z) const noexcept;
        [[nodiscard]] MeshHandle terrainMeshHandle() const noexcept;

    private:
        Terrain terrain_;
        MeshHandle terrainMesh_{};
        std::uint64_t terrainRevision_ = 0;
    };

} // namespace lumin::scene

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "scene/Camera.hpp"
#include "scene/Level.hpp"
#include "scripting/ScriptRuntime.hpp"

namespace lumin::project {

    enum class AssetType {
        Mesh,
        Texture,
        Script,
    };

    enum class ImportConflictPolicy {
        Skip,
        Replace,
        Rename,
    };

    struct AssetId {
        std::string value;

        [[nodiscard]] bool isValid() const noexcept;
        friend bool operator==(const AssetId&, const AssetId&) = default;
    };

    struct AssetRecord {
        AssetId id;
        AssetType type = AssetType::Mesh;
        std::filesystem::path relativePath;
        std::string displayName;
    };

    class AssetRegistry {
    public:
        [[nodiscard]] const std::vector<AssetRecord>& assets() const noexcept;
        [[nodiscard]] const AssetRecord* find(const AssetId& id) const noexcept;
        [[nodiscard]] const AssetRecord* findByPath(const std::filesystem::path& relativePath) const noexcept;
        void addOrReplace(AssetRecord record);
        bool remove(const AssetId& id);
        void clear() noexcept;

    private:
        std::vector<AssetRecord> assets_;
    };

    struct ImportRequest {
        std::filesystem::path source;
        std::filesystem::path destinationDirectory;
        ImportConflictPolicy conflict = ImportConflictPolicy::Rename;
    };

    struct ImportItemResult {
        std::filesystem::path source;
        std::optional<AssetRecord> asset;
        std::string error;

        [[nodiscard]] bool succeeded() const noexcept {
            return asset.has_value();
        }
    };

    struct ProjectManifest {
        std::uint32_t formatVersion = 1;
        std::string name;
        std::filesystem::path contentDirectory = "Content";
        std::filesystem::path defaultScene = "Scenes/Main.lumin.scene";
    };

    struct ProjectRenderSettings {
        bool directLighting = true;
        bool shadows = true;
        bool rayTracing = true;
        bool ssao = true;
        bool sharc = true;
        bool nrd = true;
        bool taa = true;
        float splitLambda = 0.68f;
        float shadowDistance = 200.0f;
        float exposure = 1.0f;
    };

    class ProjectSession {
    public:
        ProjectSession(scene::Level& level, scene::Camera& camera, scripting::ScriptRuntime& scripts);

        bool create(const std::filesystem::path& location, std::string_view name, std::string& error);
        bool open(const std::filesystem::path& projectFile, std::string& error);
        bool save(std::string& error);
        void close();

        [[nodiscard]] bool hasProject() const noexcept;
        [[nodiscard]] bool dirty() const noexcept;
        void markDirty() noexcept;
        [[nodiscard]] const std::filesystem::path& projectFile() const noexcept;
        [[nodiscard]] const std::filesystem::path& rootDirectory() const noexcept;
        [[nodiscard]] const ProjectManifest& manifest() const noexcept;
        [[nodiscard]] AssetRegistry& assets() noexcept;
        [[nodiscard]] const AssetRegistry& assets() const noexcept;
        [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept;

        [[nodiscard]] std::vector<ImportItemResult> importAssets(std::span<const ImportRequest> requests);
        bool renameAsset(const AssetId& asset, std::string_view newName, std::string& error);
        bool removeAsset(const AssetId& asset, std::string& error);
        [[nodiscard]] std::optional<scene::ActorHandle> createActorFromMesh(const AssetId& asset,
                                                                            scene::Transform transform = {});
        [[nodiscard]] std::optional<scene::MeshHandle> meshForAsset(const AssetId& asset);
        [[nodiscard]] std::optional<AssetId> assetForMesh(scene::MeshHandle mesh) const;

        void setRenderSettings(ProjectRenderSettings settings) noexcept;
        [[nodiscard]] const ProjectRenderSettings& renderSettings() const noexcept;

    private:
        scene::Level& level_;
        scene::Camera& camera_;
        scripting::ScriptRuntime& scripts_;
        ProjectManifest manifest_;
        AssetRegistry assets_;
        std::filesystem::path projectFile_;
        std::filesystem::path root_;
        std::unordered_map<std::string, scene::MeshHandle> loadedMeshes_;
        std::vector<std::string> diagnostics_;
        ProjectRenderSettings renderSettings_;
        bool dirty_ = false;
    };

    [[nodiscard]] AssetId generateAssetId();
    [[nodiscard]] std::optional<AssetType> assetTypeForPath(const std::filesystem::path& path) noexcept;
    [[nodiscard]] const char* assetTypeName(AssetType type) noexcept;

} // namespace lumin::project

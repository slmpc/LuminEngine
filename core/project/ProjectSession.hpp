#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

    struct AssetId {
        std::string value;

        [[nodiscard]] bool isValid() const noexcept;
        friend bool operator==(const AssetId&, const AssetId&) = default;
    };

    struct AssetFingerprint {
        std::uintmax_t fileSize = 0;
        std::uint64_t contentHash = 0;
        bool valid = false;

        friend bool operator==(const AssetFingerprint&, const AssetFingerprint&) = default;
    };

    struct AssetRecord {
        AssetId id;
        AssetType type = AssetType::Mesh;
        std::filesystem::path relativePath;
        std::string displayName;
        AssetFingerprint fingerprint;
        bool available = false;

        friend bool operator==(const AssetRecord&, const AssetRecord&) = default;
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

    enum class ProjectEntryKind {
        Directory,
        File,
    };

    struct ProjectEntry {
        std::filesystem::path relativePath;
        ProjectEntryKind kind = ProjectEntryKind::File;
        bool protectedEntry = false;
        std::optional<AssetId> asset;

        friend bool operator==(const ProjectEntry&, const ProjectEntry&) = default;
    };

    struct AssetSyncResult {
        std::size_t added = 0;
        std::size_t moved = 0;
        std::size_t modified = 0;
        std::size_t missing = 0;
        std::vector<std::string> diagnostics;
        std::string error;

        [[nodiscard]] bool succeeded() const noexcept {
            return error.empty();
        }

        [[nodiscard]] bool changed() const noexcept {
            return added != 0 || moved != 0 || modified != 0 || missing != 0;
        }
    };

    struct ProjectManifest {
        std::uint32_t formatVersion = 1;
        std::string name;
        std::filesystem::path contentDirectory = "Content";
        std::filesystem::path defaultScene = "Scenes/Main.lumin.scene";
    };

    enum class ProjectAmbientOcclusionMode {
        Ssao,
        Hbao,
        Gtao,
    };

    struct ProjectRenderSettings {
        bool directLighting = true;
        bool shadows = true;
        bool rayTracing = true;
        bool ssao = true;
        ProjectAmbientOcclusionMode ambientOcclusionMode = ProjectAmbientOcclusionMode::Ssao;
        float ambientOcclusionRadius = 1.0f;
        float ambientOcclusionStrength = 1.0f;
        float ambientOcclusionBias = 0.08f;
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
        [[nodiscard]] const std::vector<ProjectEntry>& projectEntries() const noexcept;
        [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept;

        [[nodiscard]] AssetSyncResult synchronizeProjectFiles(bool forceHash = false);
        bool renameAsset(const AssetId& asset, std::string_view newName, std::string& error);
        bool removeAsset(const AssetId& asset, std::string& error);
        [[nodiscard]] std::optional<scene::ActorHandle> createActorFromMesh(const AssetId& asset,
                                                                            scene::Transform transform = {});
        [[nodiscard]] std::optional<scene::MeshHandle> meshForAsset(const AssetId& asset);
        [[nodiscard]] std::optional<AssetId> assetForMesh(scene::MeshHandle mesh) const;

        void setRenderSettings(ProjectRenderSettings settings) noexcept;
        [[nodiscard]] const ProjectRenderSettings& renderSettings() const noexcept;

    private:
        struct ObservedAssetFile {
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t fileSize = 0;
            AssetFingerprint fingerprint;
        };

        scene::Level& level_;
        scene::Camera& camera_;
        scripting::ScriptRuntime& scripts_;
        ProjectManifest manifest_;
        AssetRegistry assets_;
        std::filesystem::path projectFile_;
        std::filesystem::path root_;
        std::unordered_map<std::string, scene::MeshHandle> loadedMeshes_;
        std::unordered_map<std::string, ObservedAssetFile> observedAssetFiles_;
        std::vector<ProjectEntry> projectEntries_;
        std::vector<std::string> diagnostics_;
        ProjectRenderSettings renderSettings_;
        bool registryNeedsUpgrade_ = false;
        bool dirty_ = false;
    };

    [[nodiscard]] AssetId generateAssetId();
    [[nodiscard]] std::optional<AssetType> assetTypeForPath(const std::filesystem::path& path) noexcept;
    [[nodiscard]] const char* assetTypeName(AssetType type) noexcept;

} // namespace lumin::project

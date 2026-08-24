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

    /** 项目逻辑更新频率的默认值。 */
    inline constexpr std::uint32_t DefaultLogicTickHz = 60;
    /** 项目允许的最低逻辑更新频率。 */
    inline constexpr std::uint32_t MinimumLogicTickHz = 15;
    /** 项目允许的最高逻辑更新频率。 */
    inline constexpr std::uint32_t MaximumLogicTickHz = 240;

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
        /** FSR1 RCAS 锐度，范围为 `[0, 1]`。 */
        float taaSharpness = 0.5f;
        float splitLambda = 0.68f;
        float shadowDistance = 200.0f;
        float exposure = 1.0f;

        friend bool operator==(const ProjectRenderSettings&, const ProjectRenderSettings&) = default;
    };

    /**
     * @brief 随项目场景持久化的运行设置。
     *
     * 设置由拥有 `ProjectSession` 的逻辑线程修改；快照消费者只能读取副本。
     */
    struct ProjectSettings {
        /** 固定逻辑 Tick 频率，单位为 Hz，始终归一化到支持范围。 */
        std::uint32_t logicTickHz = DefaultLogicTickHz;
        /** 项目的渲染设置。 */
        ProjectRenderSettings render;

        friend bool operator==(const ProjectSettings&, const ProjectSettings&) = default;
    };

    /** 将项目设置归一化到引擎支持范围。 */
    void normalizeProjectSettings(ProjectSettings& settings) noexcept;

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
        /**
         * 从 Mesh 资源创建一个或多个 Actor。
         *
         * 单材质 OBJ 创建一个
         * Actor；多材质

         * * OBJ 按稳定的材质分区创建多个 Actor，并为每个 Actor 应用 MTL 材质。
         */
        [[nodiscard]] std::vector<scene::ActorHandle> createActorsFromMesh(const AssetId& asset,
                                                                           scene::Transform transform = {});
        /**
         * 从 Mesh 资源创建 Actor，并返回第一个 Actor。
         *
         *
         * 为兼容旧调用方保留；多材质

         * * OBJ 的其余 Actor 也会同时创建并由当前场景拥有。
         */
        [[nodiscard]] std::optional<scene::ActorHandle> createActorFromMesh(const AssetId& asset,
                                                                            scene::Transform transform = {});
        /** 创建持久化局部光 Actor，自动分配 ID、默认名称并标记项目已修改。 */
        [[nodiscard]] scene::ActorHandle createLightActor(scene::LocalLight light, scene::Transform transform = {});
        /** 返回 Mesh 资源的首个材质分区，供只支持单网格的兼容调用方使用。 */
        [[nodiscard]] std::optional<scene::MeshHandle> meshForAsset(const AssetId& asset);
        /** 返回运行时 Mesh 所属的项目资源。 */
        [[nodiscard]] std::optional<AssetId> assetForMesh(scene::MeshHandle mesh) const;

        /** 替换完整项目设置并推进 dirty 状态。 */
        void setSettings(ProjectSettings settings) noexcept;
        /** 返回当前完整项目设置。 */
        [[nodiscard]] const ProjectSettings& settings() const noexcept;
        /** 替换项目渲染设置并推进 dirty 状态。 */
        void setRenderSettings(ProjectRenderSettings settings) noexcept;
        /** 返回当前项目渲染设置。 */
        [[nodiscard]] const ProjectRenderSettings& renderSettings() const noexcept;

    private:
        struct ObservedAssetFile {
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t fileSize = 0;
            AssetFingerprint fingerprint;
        };

        struct LoadedMeshPart {
            std::string name;
            scene::MeshHandle mesh;
            scene::Material material;
        };

        struct MeshAssetReference {
            AssetId asset;
            std::string part;
        };

        [[nodiscard]] const std::vector<LoadedMeshPart>* meshPartsForAsset(const AssetId& asset);
        [[nodiscard]] const LoadedMeshPart* meshPartForAsset(const AssetId& asset, std::string_view part);
        [[nodiscard]] std::optional<MeshAssetReference> meshReferenceFor(scene::MeshHandle mesh) const;

        scene::Level& level_;
        scene::Camera& camera_;
        scripting::ScriptRuntime& scripts_;
        ProjectManifest manifest_;
        AssetRegistry assets_;
        std::filesystem::path projectFile_;
        std::filesystem::path root_;
        std::unordered_map<std::string, std::vector<LoadedMeshPart>> loadedMeshes_;
        std::unordered_map<std::string, ObservedAssetFile> observedAssetFiles_;
        std::vector<ProjectEntry> projectEntries_;
        std::vector<std::string> diagnostics_;
        ProjectSettings settings_;
        bool registryNeedsUpgrade_ = false;
        bool dirty_ = false;
    };

    [[nodiscard]] AssetId generateAssetId();
    [[nodiscard]] std::optional<AssetType> assetTypeForPath(const std::filesystem::path& path) noexcept;
    [[nodiscard]] const char* assetTypeName(AssetType type) noexcept;

} // namespace lumin::project

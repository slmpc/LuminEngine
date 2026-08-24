#include "project/ProjectSession.hpp"

#include "assets/ImageLoader.hpp"
#include "assets/ObjLoader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace lumin::project {
    namespace {
        using Json = nlohmann::json;
        constexpr std::uint32_t formatVersion = 1;
        constexpr std::uint32_t registryFormatVersion = 2;
        constexpr std::uint64_t fnvOffset = 14695981039346656037ULL;
        constexpr std::uint64_t fnvPrime = 1099511628211ULL;

        [[nodiscard]] ProjectAmbientOcclusionMode parseAmbientOcclusionMode(const Json& settings) {
            const std::string mode = settings.value("ambientOcclusionMode", std::string{"ssao"});
            if (mode == "hbao") {
                return ProjectAmbientOcclusionMode::Hbao;
            }
            if (mode == "gtao") {
                return ProjectAmbientOcclusionMode::Gtao;
            }
            return ProjectAmbientOcclusionMode::Ssao;
        }

        [[nodiscard]] const char* ambientOcclusionModeName(ProjectAmbientOcclusionMode mode) noexcept {
            switch (mode) {
            case ProjectAmbientOcclusionMode::Hbao:
                return "hbao";
            case ProjectAmbientOcclusionMode::Gtao:
                return "gtao";
            case ProjectAmbientOcclusionMode::Ssao:
            default:
                return "ssao";
            }
        }

        struct DiscoveredAsset {
            std::filesystem::path relativePath;
            AssetType type = AssetType::Mesh;
            AssetFingerprint fingerprint;
            bool matched = false;
        };

        Json vectorJson(const glm::vec3& value) {
            return Json::array({value.x, value.y, value.z});
        }

        glm::vec3 readVector(const Json& value, const glm::vec3& fallback = {}) {
            if (!value.is_array() || value.size() != 3) {
                return fallback;
            }
            return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
        }

        Json localLightJson(const scene::LocalLight& light) {
            return std::visit(
                [](const auto& value) {
                    using Light = std::remove_cvref_t<decltype(value)>;
                    Json result{{"type", std::is_same_v<Light, scene::PointLight> ? "point" : "spot"},
                                {"enabled", value.enabled},
                                {"color", vectorJson(value.color)},
                                {"luminousIntensityCandela", value.luminousIntensityCandela},
                                {"range", value.range},
                                {"castsShadows", value.castsShadows}};
                    if constexpr (std::is_same_v<Light, scene::SpotLight>) {
                        result["innerConeAngleDegrees"] = value.innerConeAngleDegrees;
                        result["outerConeAngleDegrees"] = value.outerConeAngleDegrees;
                    }
                    return result;
                },
                light);
        }

        scene::LocalLight readLocalLight(const Json& value) {
            const std::string type = value.value("type", std::string{});
            if (type == "point") {
                scene::PointLight light;
                light.enabled = value.value("enabled", light.enabled);
                light.color = readVector(value.value("color", Json{}), light.color);
                light.luminousIntensityCandela =
                    value.value("luminousIntensityCandela", light.luminousIntensityCandela);
                light.range = value.value("range", light.range);
                light.castsShadows = value.value("castsShadows", light.castsShadows);
                return light;
            }
            if (type == "spot") {
                scene::SpotLight light;
                light.enabled = value.value("enabled", light.enabled);
                light.color = readVector(value.value("color", Json{}), light.color);
                light.luminousIntensityCandela =
                    value.value("luminousIntensityCandela", light.luminousIntensityCandela);
                light.range = value.value("range", light.range);
                light.castsShadows = value.value("castsShadows", light.castsShadows);
                light.innerConeAngleDegrees = value.value("innerConeAngleDegrees", light.innerConeAngleDegrees);
                light.outerConeAngleDegrees = value.value("outerConeAngleDegrees", light.outerConeAngleDegrees);
                return light;
            }
            throw std::invalid_argument("Actor light type must be 'point' or 'spot'.");
        }

        const char* typeName(AssetType type) {
            switch (type) {
            case AssetType::Mesh:
                return "Mesh";
            case AssetType::Texture:
                return "Texture";
            case AssetType::Script:
                return "Script";
            }
            return "Unknown";
        }

        std::optional<AssetType> parseType(std::string_view value) {
            if (value == "Mesh") {
                return AssetType::Mesh;
            }
            if (value == "Texture") {
                return AssetType::Texture;
            }
            if (value == "Script") {
                return AssetType::Script;
            }
            return std::nullopt;
        }

        std::string hashString(std::uint64_t value) {
            std::ostringstream stream;
            stream << std::hex << std::setfill('0') << std::setw(16) << value;
            return stream.str();
        }

        std::optional<std::uint64_t> parseHash(std::string_view value) {
            std::uint64_t result = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
            if (error != std::errc{} || end != value.data() + value.size()) {
                return std::nullopt;
            }
            return result;
        }

        std::string fingerprintKey(AssetType type, const AssetFingerprint& fingerprint) {
            return std::to_string(static_cast<unsigned int>(type)) + ":" + std::to_string(fingerprint.fileSize) + ":" +
                   hashString(fingerprint.contentHash);
        }

        AssetFingerprint fingerprintFile(const std::filesystem::path& path) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                throw std::runtime_error("Could not read asset file '" + path.generic_string() + "'.");
            }
            std::uint64_t hash = fnvOffset;
            std::uintmax_t size = 0;
            std::array<char, 64 * 1024> buffer{};
            while (stream) {
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize count = stream.gcount();
                for (std::streamsize index = 0; index < count; ++index) {
                    hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
                    hash *= fnvPrime;
                }
                size += static_cast<std::uintmax_t>(count);
            }
            if (stream.bad()) {
                throw std::runtime_error("Failed while reading asset file '" + path.generic_string() + "'.");
            }
            return {.fileSize = size, .contentHash = hash, .valid = true};
        }

        bool isTransientProjectFile(const std::filesystem::path& path) {
            const std::string extension = path.extension().string();
            return extension == ".tmp" || extension == ".bak" || extension == ".importing";
        }

        bool isProtectedProjectEntry(const std::filesystem::path& relative, const ProjectManifest& manifest,
                                     const std::filesystem::path& projectFile) {
            if (!relative.empty() && *relative.begin() == std::filesystem::path{".lumin"}) {
                return true;
            }
            return relative == manifest.defaultScene || relative.filename() == projectFile.filename() ||
                   relative.extension() == ".luminproject" || relative.extension() == ".scene";
        }

        bool hasInvalidProjectNameCharacter(char value) {
            constexpr std::string_view invalid = "<>:\"/\\|?*";
            return static_cast<unsigned char>(value) < 32 || invalid.find(value) != std::string_view::npos;
        }

        bool pathInside(const std::filesystem::path& root, const std::filesystem::path& path) {
            std::error_code error;
            const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
            if (error) {
                return false;
            }
            const auto canonicalParent = std::filesystem::weakly_canonical(path.parent_path(), error);
            if (error) {
                return false;
            }
            const auto canonicalPath = (canonicalParent / path.filename()).lexically_normal();
            const auto relative = canonicalPath.lexically_relative(canonicalRoot);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
        }

        bool existingPathInside(const std::filesystem::path& root, const std::filesystem::path& path) {
            std::error_code error;
            const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
            if (error) {
                return false;
            }
            const auto canonicalPath = std::filesystem::weakly_canonical(path, error);
            if (error) {
                return false;
            }
            const auto relative = canonicalPath.lexically_relative(canonicalRoot);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
        }

        std::filesystem::path checkedProjectPath(const std::filesystem::path& root,
                                                 const std::filesystem::path& relative) {
            if (relative.empty() || relative.is_absolute()) {
                throw std::runtime_error("Project paths must be non-empty and relative.");
            }
            const std::filesystem::path result = root / relative;
            if (!pathInside(root, result)) {
                throw std::runtime_error("Project path escapes the project root: " + relative.generic_string());
            }
            return result;
        }

        bool writeJsonAtomic(const std::filesystem::path& path, const Json& value, std::string& error) {
            const std::filesystem::path temporary = path.string() + ".tmp";
            const std::filesystem::path backup = path.string() + ".bak";
            try {
                std::filesystem::create_directories(path.parent_path());
                {
                    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                    if (!stream) {
                        throw std::runtime_error("Could not create temporary file.");
                    }
                    stream << value.dump(2) << '\n';
                    if (!stream) {
                        throw std::runtime_error("Could not write complete JSON data.");
                    }
                }
                std::error_code ignored;
                std::filesystem::remove(backup, ignored);
                if (std::filesystem::exists(path)) {
                    std::filesystem::rename(path, backup);
                }
                try {
                    std::filesystem::rename(temporary, path);
                    std::filesystem::remove(backup, ignored);
                } catch (...) {
                    if (std::filesystem::exists(backup)) {
                        std::filesystem::rename(backup, path, ignored);
                    }
                    throw;
                }
                return true;
            } catch (const std::exception& exception) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                error = "Failed to save '" + path.generic_string() + "': " + exception.what();
                return false;
            }
        }

        Json readJson(const std::filesystem::path& path) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                throw std::runtime_error("Could not open '" + path.generic_string() + "'.");
            }
            Json value;
            stream >> value;
            return value;
        }

        Json materialJson(const scene::Material& material, const std::filesystem::path& root) {
            Json value{{"albedo", vectorJson(material.albedo)},
                       {"surfaceModel", static_cast<std::uint32_t>(material.surfaceModel)},
                       {"roughness", material.metallicRoughness.roughness},
                       {"metallic", material.metallicRoughness.metallic},
                       {"specular", vectorJson(material.blinnPhong.specularColor)},
                       {"shininess", material.blinnPhong.shininess},
                       {"ior", material.blinnPhong.indexOfRefraction},
                       {"textureScale", material.textureScale}};
            if (material.textures.has_value() && !material.textures->empty()) {
                const auto makeRelative = [&root](const std::filesystem::path& path) {
                    if (path.empty()) {
                        return std::string{};
                    }
                    std::error_code error;
                    const auto relative = std::filesystem::weakly_canonical(path, error).lexically_relative(root);
                    return error || relative.empty() || *relative.begin() == ".." ? std::string{}
                                                                                  : relative.generic_string();
                };
                value["textures"] = {{"baseColor", makeRelative(material.textures->baseColor)},
                                     {"normal", makeRelative(material.textures->normal)},
                                     {"roughness", makeRelative(material.textures->roughness)},
                                     {"flipNormalY", material.textures->flipNormalY}};
            }
            return value;
        }

        scene::Material readMaterial(const Json& value, const std::filesystem::path& root) {
            scene::Material material;
            material.albedo = readVector(value.value("albedo", Json{}), material.albedo);
            material.surfaceModel = static_cast<scene::SurfaceModel>(value.value("surfaceModel", 0U));
            material.metallicRoughness.roughness = value.value("roughness", material.metallicRoughness.roughness);
            material.metallicRoughness.metallic = value.value("metallic", material.metallicRoughness.metallic);
            material.blinnPhong.specularColor =
                readVector(value.value("specular", Json{}), material.blinnPhong.specularColor);
            material.blinnPhong.shininess = value.value("shininess", material.blinnPhong.shininess);
            material.blinnPhong.indexOfRefraction = value.value("ior", material.blinnPhong.indexOfRefraction);
            material.textureScale = value.value("textureScale", material.textureScale);
            if (const auto iterator = value.find("textures"); iterator != value.end() && iterator->is_object()) {
                scene::PbrTextureSet textures;
                const auto resolve = [&root](const std::string& path) {
                    return path.empty() ? std::filesystem::path{} : root / std::filesystem::path{path};
                };
                textures.baseColor = resolve(iterator->value("baseColor", std::string{}));
                textures.normal = resolve(iterator->value("normal", std::string{}));
                textures.roughness = resolve(iterator->value("roughness", std::string{}));
                textures.flipNormalY = iterator->value("flipNormalY", true);
                if (!textures.empty()) {
                    material.textures = std::move(textures);
                }
            }
            return material;
        }

        scene::Material importedMaterial(const assets::ObjMaterial& source, const std::filesystem::path& root,
                                         std::vector<std::string>& diagnostics) {
            scene::Material material;
            material.albedo = source.diffuseColor;
            material.surfaceModel = scene::SurfaceModel::BlinnPhong;
            material.blinnPhong.specularColor = source.specularColor;
            material.blinnPhong.shininess = source.shininess;
            material.blinnPhong.indexOfRefraction = source.indexOfRefraction;

            const auto validatedTexture = [&root, &diagnostics,
                                           &source](const std::filesystem::path& texture) -> std::filesystem::path {
                if (texture.empty()) {
                    return {};
                }
                std::error_code error;
                const bool usable = std::filesystem::is_regular_file(texture, error) && !error &&
                                    existingPathInside(root, texture) &&
                                    assetTypeForPath(texture).value_or(AssetType::Mesh) == AssetType::Texture;
                if (!usable) {
                    diagnostics.push_back("OBJ material '" + source.name + "' references unavailable texture '" +
                                          texture.generic_string() + "'.");
                    return {};
                }
                return texture;
            };

            scene::PbrTextureSet textures{
                .baseColor = validatedTexture(source.diffuseTexture),
                .normal = validatedTexture(source.normalTexture),
                .roughness = validatedTexture(source.roughnessTexture),
                .flipNormalY = false,
            };
            if (!textures.empty()) {
                material.textures = std::move(textures);
            }
            return material;
        }

        Json registryJson(const AssetRegistry& registry) {
            Json assets = Json::array();
            for (const AssetRecord& asset : registry.assets()) {
                Json item{{"id", asset.id.value},
                          {"type", typeName(asset.type)},
                          {"path", asset.relativePath.generic_string()},
                          {"name", asset.displayName}};
                if (asset.fingerprint.valid) {
                    item["fingerprint"] = {{"size", asset.fingerprint.fileSize},
                                           {"hash", hashString(asset.fingerprint.contentHash)}};
                }
                assets.push_back(std::move(item));
            }
            return {{"formatVersion", registryFormatVersion}, {"assets", std::move(assets)}};
        }

        AssetRegistry parseRegistry(const Json& value, const std::filesystem::path& root) {
            const std::uint32_t version = value.value("formatVersion", 0U);
            if ((version != 1 && version != registryFormatVersion) || !value.contains("assets") ||
                !value["assets"].is_array()) {
                throw std::runtime_error("Unsupported or malformed asset registry.");
            }
            AssetRegistry registry;
            for (const Json& item : value["assets"]) {
                const auto type = parseType(item.value("type", std::string{}));
                AssetFingerprint fingerprint;
                if (version >= registryFormatVersion) {
                    const auto iterator = item.find("fingerprint");
                    if (iterator != item.end() && iterator->is_object()) {
                        const auto hash = parseHash(iterator->value("hash", std::string{}));
                        if (hash.has_value()) {
                            fingerprint = {.fileSize = iterator->value("size", std::uintmax_t{}),
                                           .contentHash = *hash,
                                           .valid = true};
                        }
                    }
                }
                AssetRecord record{{item.value("id", std::string{})},
                                   type.value_or(AssetType::Mesh),
                                   item.value("path", std::string{}),
                                   item.value("name", std::string{}),
                                   fingerprint,
                                   false};
                if (!record.id.isValid() || !type.has_value()) {
                    throw std::runtime_error("Asset registry contains an invalid asset record.");
                }
                static_cast<void>(checkedProjectPath(root, record.relativePath));
                registry.addOrReplace(std::move(record));
            }
            return registry;
        }

    } // namespace

    void normalizeProjectSettings(ProjectSettings& settings) noexcept {
        settings.logicTickHz = std::clamp(settings.logicTickHz, MinimumLogicTickHz, MaximumLogicTickHz);
        settings.render.taaSharpness = std::clamp(settings.render.taaSharpness, 0.0f, 1.0f);
    }

    bool AssetId::isValid() const noexcept {
        return value.size() == 32 && std::ranges::all_of(value, [](unsigned char character) {
                   return std::isxdigit(character) != 0;
               });
    }

    const std::vector<AssetRecord>& AssetRegistry::assets() const noexcept {
        return assets_;
    }

    const AssetRecord* AssetRegistry::find(const AssetId& id) const noexcept {
        const auto iterator = std::ranges::find(assets_, id, &AssetRecord::id);
        return iterator == assets_.end() ? nullptr : &*iterator;
    }

    const AssetRecord* AssetRegistry::findByPath(const std::filesystem::path& relativePath) const noexcept {
        const auto iterator = std::ranges::find(assets_, relativePath.lexically_normal(), [](const AssetRecord& value) {
            return value.relativePath.lexically_normal();
        });
        return iterator == assets_.end() ? nullptr : &*iterator;
    }

    void AssetRegistry::addOrReplace(AssetRecord record) {
        if (!record.id.isValid() || record.relativePath.empty() || record.relativePath.is_absolute()) {
            throw std::invalid_argument("AssetRecord requires a valid ID and relative path.");
        }
        if (AssetRecord* existing = const_cast<AssetRecord*>(find(record.id)); existing != nullptr) {
            *existing = std::move(record);
            return;
        }
        assets_.push_back(std::move(record));
    }

    bool AssetRegistry::remove(const AssetId& id) {
        return std::erase_if(assets_, [&id](const AssetRecord& value) {
                   return value.id == id;
               }) != 0;
    }

    void AssetRegistry::clear() noexcept {
        assets_.clear();
    }

    AssetId generateAssetId() {
        std::random_device random;
        std::uniform_int_distribution<unsigned int> distribution(0, 255);
        constexpr char hex[] = "0123456789abcdef";
        std::string value(32, '0');
        for (std::size_t index = 0; index < 16; ++index) {
            const unsigned int byte = distribution(random);
            value[index * 2] = hex[byte >> 4U];
            value[index * 2 + 1] = hex[byte & 0x0fU];
        }
        return AssetId{std::move(value)};
    }

    std::optional<AssetType> assetTypeForPath(const std::filesystem::path& path) noexcept {
        std::string extension = path.extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (extension == ".obj") {
            return AssetType::Mesh;
        }
        if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
            return AssetType::Texture;
        }
        if (extension == ".lua") {
            return AssetType::Script;
        }
        return std::nullopt;
    }

    const char* assetTypeName(AssetType type) noexcept {
        return typeName(type);
    }

    ProjectSession::ProjectSession(scene::Level& level, scene::Camera& camera, scripting::ScriptRuntime& scripts)
        : level_(level), camera_(camera), scripts_(scripts) {
    }

    bool ProjectSession::create(const std::filesystem::path& location, std::string_view name, std::string& error) {
        try {
            if (name.empty() || std::ranges::any_of(name, hasInvalidProjectNameCharacter)) {
                throw std::runtime_error("Project name is empty or contains an invalid path character.");
            }
            const std::filesystem::path nextRoot = std::filesystem::absolute(location / std::filesystem::path{name});
            if (std::filesystem::exists(nextRoot) && !std::filesystem::is_empty(nextRoot)) {
                throw std::runtime_error("Project directory already exists and is not empty.");
            }
            std::filesystem::create_directories(nextRoot / "Content/Meshes");
            std::filesystem::create_directories(nextRoot / "Content/Textures");
            std::filesystem::create_directories(nextRoot / "Content/Scripts");
            std::filesystem::create_directories(nextRoot / "Scenes");
            std::filesystem::create_directories(nextRoot / ".lumin");

            level_.clear();
            level_.setEnvironment(scene::SceneEnvironment{});
            camera_ = scene::Camera{};
            camera_.markCut();
            settings_ = {};
            if (!scripts_.setScriptRoot(nextRoot)) {
                throw std::runtime_error("Could not switch the script root while scripts are active.");
            }
            root_ = nextRoot;
            projectFile_ = root_ / (std::string{name} + ".luminproject");
            manifest_ = ProjectManifest{formatVersion, std::string{name}, "Content", "Scenes/Main.lumin.scene"};
            assets_.clear();
            loadedMeshes_.clear();
            observedAssetFiles_.clear();
            projectEntries_.clear();
            diagnostics_.clear();
            registryNeedsUpgrade_ = false;
            dirty_ = true;
            if (!save(error)) {
                return false;
            }
            const AssetSyncResult sync = synchronizeProjectFiles();
            if (!sync.succeeded()) {
                throw std::runtime_error(sync.error);
            }
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    bool ProjectSession::open(const std::filesystem::path& projectFile, std::string& error) {
        try {
            const std::filesystem::path nextProjectFile = std::filesystem::absolute(projectFile);
            const std::filesystem::path nextRoot = nextProjectFile.parent_path();
            const Json manifestJson = readJson(nextProjectFile);
            ProjectManifest nextManifest;
            nextManifest.formatVersion = manifestJson.value("formatVersion", 0U);
            nextManifest.name = manifestJson.value("name", std::string{});
            nextManifest.contentDirectory = manifestJson.value("contentDirectory", std::string{});
            nextManifest.defaultScene = manifestJson.value("defaultScene", std::string{});
            if (nextManifest.formatVersion != formatVersion || nextManifest.name.empty()) {
                throw std::runtime_error("Unsupported or malformed project manifest.");
            }
            static_cast<void>(checkedProjectPath(nextRoot, nextManifest.contentDirectory));
            const std::filesystem::path scenePath = checkedProjectPath(nextRoot, nextManifest.defaultScene);
            const Json registryDocument = readJson(checkedProjectPath(nextRoot, ".lumin/AssetRegistry.json"));
            const AssetRegistry nextRegistry = parseRegistry(registryDocument, nextRoot);
            const bool nextRegistryNeedsUpgrade = registryDocument.value("formatVersion", 0U) < registryFormatVersion;
            const Json sceneJson = readJson(scenePath);
            if (sceneJson.value("formatVersion", 0U) != formatVersion || !sceneJson.contains("actors") ||
                !sceneJson["actors"].is_array()) {
                throw std::runtime_error("Unsupported or malformed scene document.");
            }

            level_.clear();
            if (!scripts_.setScriptRoot(nextRoot)) {
                throw std::runtime_error("Could not switch the script root while scripts are active.");
            }
            root_ = nextRoot;
            projectFile_ = nextProjectFile;
            manifest_ = std::move(nextManifest);
            assets_ = nextRegistry;
            registryNeedsUpgrade_ = nextRegistryNeedsUpgrade;
            loadedMeshes_.clear();
            observedAssetFiles_.clear();
            projectEntries_.clear();
            diagnostics_.clear();

            const AssetSyncResult sync = synchronizeProjectFiles();
            if (!sync.succeeded()) {
                throw std::runtime_error(sync.error);
            }

            if (const auto camera = sceneJson.find("camera"); camera != sceneJson.end() && camera->is_object()) {
                camera_.setPosition(readVector(camera->value("position", Json{}), camera_.position()));
                camera_.setOrientation(camera->value("yaw", camera_.yawDegrees()),
                                       camera->value("pitch", camera_.pitchDegrees()));
                camera_.setMoveSpeed(camera->value("speed", camera_.moveSpeed()));
                camera_.markCut();
            }
            if (const auto environment = sceneJson.find("environment");
                environment != sceneJson.end() && environment->is_object()) {
                scene::SceneEnvironment value = level_.environment();
                value.sun.direction = readVector(environment->value("sunDirection", Json{}), value.sun.direction);
                value.sun.color = readVector(environment->value("sunColor", Json{}), value.sun.color);
                value.sun.illuminanceLux = environment->value("illuminanceLux", value.sun.illuminanceLux);
                value.sun.castsShadows = environment->value("castsShadows", value.sun.castsShadows);
                level_.setEnvironment(value);
            }
            settings_ = {};
            if (const auto projectSettings = sceneJson.find("projectSettings");
                projectSettings != sceneJson.end() && projectSettings->is_object()) {
                settings_.logicTickHz = projectSettings->value("logicTickHz", DefaultLogicTickHz);
            }
            const auto renderSettings = sceneJson.find("renderSettings");
            const Json* render =
                renderSettings != sceneJson.end() && renderSettings->is_object() ? &*renderSettings : nullptr;
            if (const auto projectSettings = sceneJson.find("projectSettings");
                projectSettings != sceneJson.end() && projectSettings->is_object()) {
                if (const auto nested = projectSettings->find("render");
                    nested != projectSettings->end() && nested->is_object()) {
                    render = &*nested;
                }
            }
            if (render != nullptr) {
                settings_.render.directLighting = render->value("directLighting", true);
                settings_.render.shadows = render->value("shadows", true);
                settings_.render.rayTracing = render->value("rayTracing", true);
                settings_.render.ssao = render->value("ssao", true);
                settings_.render.ambientOcclusionMode = parseAmbientOcclusionMode(*render);
                settings_.render.ambientOcclusionRadius = render->value("ambientOcclusionRadius", 1.0f);
                settings_.render.ambientOcclusionStrength = render->value("ambientOcclusionStrength", 1.0f);
                settings_.render.ambientOcclusionBias = render->value("ambientOcclusionBias", 0.08f);
                settings_.render.sharc = render->value("sharc", true);
                settings_.render.nrd = render->value("nrd", true);
                settings_.render.taa = render->value("taa", true);
                settings_.render.taaSharpness = render->value("taaSharpness", 0.5f);
                settings_.render.splitLambda = render->value("splitLambda", 0.68f);
                settings_.render.shadowDistance = render->value("shadowDistance", 200.0f);
                settings_.render.exposure = render->value("exposure", 1.0f);
            }
            normalizeProjectSettings(settings_);
            for (const Json& actorJson : sceneJson["actors"]) {
                const scene::ActorHandle handle = level_.spawnActor();
                scene::Actor* actor = level_.actor(handle);
                actor->setName(actorJson.value("name", std::string{"Actor"}));
                actor->setPersistentId(actorJson.value("id", std::string{}));
                scene::Transform transform;
                if (const auto transformJson = actorJson.find("transform");
                    transformJson != actorJson.end() && transformJson->is_object()) {
                    transform.position = readVector(transformJson->value("position", Json{}), transform.position);
                    transform.rotationDegrees =
                        readVector(transformJson->value("rotation", Json{}), transform.rotationDegrees);
                    transform.scale = readVector(transformJson->value("scale", Json{}), transform.scale);
                }
                actor->setTransform(transform);
                if (const auto lightJson = actorJson.find("light");
                    lightJson != actorJson.end() && lightJson->is_object()) {
                    actor->setLocalLight(readLocalLight(*lightJson));
                }
                const auto materialJson = actorJson.find("material");
                const bool hasSerializedMaterial = materialJson != actorJson.end() && materialJson->is_object();
                if (hasSerializedMaterial) {
                    actor->setMaterial(readMaterial(*materialJson, root_));
                }
                const AssetId meshAsset{actorJson.value("mesh", std::string{})};
                if (meshAsset.isValid()) {
                    const std::string meshPart = actorJson.value("meshPart", std::string{});
                    if (const LoadedMeshPart* loaded = meshPartForAsset(meshAsset, meshPart); loaded != nullptr) {
                        if (!hasSerializedMaterial) {
                            actor->setMaterial(loaded->material);
                        }
                        actor->attachModel(loaded->mesh, actor->material());
                    } else {
                        diagnostics_.push_back("Missing mesh for Actor '" + actor->name() + "'.");
                    }
                }
                if (const auto scripts = actorJson.find("scripts"); scripts != actorJson.end() && scripts->is_array()) {
                    for (const Json& scriptJson : *scripts) {
                        const AssetId scriptAsset{scriptJson.value("asset", std::string{})};
                        const AssetRecord* record = assets_.find(scriptAsset);
                        if (record == nullptr || record->type != AssetType::Script) {
                            diagnostics_.push_back("Missing script for Actor '" + actor->name() + "'.");
                            continue;
                        }
                        const auto attached = scripts_.attach(level_, handle, root_ / record->relativePath);
                        if (!attached) {
                            diagnostics_.push_back(attached.result.error.has_value() ? attached.result.error->message
                                                                                     : "Could not attach script.");
                        } else {
                            scripts_.setEnabled(attached.script, scriptJson.value("enabled", true));
                        }
                    }
                }
            }
            dirty_ = false;
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    bool ProjectSession::save(std::string& error) {
        if (!hasProject()) {
            error = "No project is open.";
            return false;
        }
        Json manifestJson{{"formatVersion", formatVersion},
                          {"name", manifest_.name},
                          {"contentDirectory", manifest_.contentDirectory.generic_string()},
                          {"defaultScene", manifest_.defaultScene.generic_string()}};
        Json actors = Json::array();
        for (const scene::ActorHandle handle : level_.actorHandles()) {
            const scene::Actor* actor = level_.actor(handle);
            if (actor == nullptr || actor->persistentId().empty()) {
                continue;
            }
            const scene::Transform& transform = actor->transform();
            Json actorJson{{"id", actor->persistentId()},
                           {"name", actor->name()},
                           {"transform",
                            {{"position", vectorJson(transform.position)},
                             {"rotation", vectorJson(transform.rotationDegrees)},
                             {"scale", vectorJson(transform.scale)}}},
                           {"material", materialJson(actor->material(), root_)}};
            if (actor->modelHandle().isValid()) {
                if (const auto meshReference = meshReferenceFor(level_.model(actor->modelHandle()).mesh);
                    meshReference.has_value()) {
                    actorJson["mesh"] = meshReference->asset.value;
                    actorJson["meshPart"] = meshReference->part;
                }
            }
            if (actor->localLight().has_value()) {
                actorJson["light"] = localLightJson(*actor->localLight());
            }
            Json scriptArray = Json::array();
            for (const scripting::ScriptInfo& script : scripts_.scriptsForActor(handle)) {
                std::error_code relativeError;
                const auto relative =
                    std::filesystem::weakly_canonical(script.source, relativeError).lexically_relative(root_);
                const AssetRecord* asset = relativeError ? nullptr : assets_.findByPath(relative);
                if (asset != nullptr) {
                    scriptArray.push_back({{"asset", asset->id.value}, {"enabled", script.enabled}});
                }
            }
            actorJson["scripts"] = std::move(scriptArray);
            actors.push_back(std::move(actorJson));
        }
        const scene::DirectionalLight& sun = level_.environment().sun;
        Json sceneJson{{"formatVersion", formatVersion},
                       {"camera",
                        {{"position", vectorJson(camera_.position())},
                         {"yaw", camera_.yawDegrees()},
                         {"pitch", camera_.pitchDegrees()},
                         {"speed", camera_.moveSpeed()}}},
                       {"environment",
                        {{"sunDirection", vectorJson(sun.direction)},
                         {"sunColor", vectorJson(sun.color)},
                         {"illuminanceLux", sun.illuminanceLux},
                         {"castsShadows", sun.castsShadows}}},
                       {"projectSettings",
                        {{"logicTickHz", settings_.logicTickHz},
                         {"render",
                          {{"directLighting", settings_.render.directLighting},
                           {"shadows", settings_.render.shadows},
                           {"rayTracing", settings_.render.rayTracing},
                           {"ssao", settings_.render.ssao},
                           {"ambientOcclusionMode", ambientOcclusionModeName(settings_.render.ambientOcclusionMode)},
                           {"ambientOcclusionRadius", settings_.render.ambientOcclusionRadius},
                           {"ambientOcclusionStrength", settings_.render.ambientOcclusionStrength},
                           {"ambientOcclusionBias", settings_.render.ambientOcclusionBias},
                           {"sharc", settings_.render.sharc},
                           {"nrd", settings_.render.nrd},
                           {"taa", settings_.render.taa},
                           {"taaSharpness", settings_.render.taaSharpness},
                           {"splitLambda", settings_.render.splitLambda},
                           {"shadowDistance", settings_.render.shadowDistance},
                           {"exposure", settings_.render.exposure}}}}},
                       {"actors", std::move(actors)}};
        if (!writeJsonAtomic(projectFile_, manifestJson, error) ||
            !writeJsonAtomic(root_ / ".lumin/AssetRegistry.json", registryJson(assets_), error) ||
            !writeJsonAtomic(root_ / manifest_.defaultScene, sceneJson, error)) {
            return false;
        }
        dirty_ = false;
        return true;
    }

    void ProjectSession::close() {
        if (hasProject()) {
            level_.clear();
        }
        projectFile_.clear();
        root_.clear();
        assets_.clear();
        loadedMeshes_.clear();
        observedAssetFiles_.clear();
        projectEntries_.clear();
        diagnostics_.clear();
        settings_ = {};
        registryNeedsUpgrade_ = false;
        dirty_ = false;
    }

    bool ProjectSession::hasProject() const noexcept {
        return !projectFile_.empty();
    }
    bool ProjectSession::dirty() const noexcept {
        return dirty_;
    }
    void ProjectSession::markDirty() noexcept {
        if (hasProject()) {
            dirty_ = true;
        }
    }
    const std::filesystem::path& ProjectSession::projectFile() const noexcept {
        return projectFile_;
    }
    const std::filesystem::path& ProjectSession::rootDirectory() const noexcept {
        return root_;
    }
    const ProjectManifest& ProjectSession::manifest() const noexcept {
        return manifest_;
    }
    AssetRegistry& ProjectSession::assets() noexcept {
        return assets_;
    }
    const AssetRegistry& ProjectSession::assets() const noexcept {
        return assets_;
    }
    const std::vector<ProjectEntry>& ProjectSession::projectEntries() const noexcept {
        return projectEntries_;
    }
    const std::vector<std::string>& ProjectSession::diagnostics() const noexcept {
        return diagnostics_;
    }

    AssetSyncResult ProjectSession::synchronizeProjectFiles(bool forceHash) {
        AssetSyncResult result;
        if (!hasProject()) {
            result.error = "No project is open.";
            return result;
        }

        try {
            std::vector<ProjectEntry> nextEntries;
            std::vector<DiscoveredAsset> discovered;
            std::unordered_map<std::string, ObservedAssetFile> nextObservedAssetFiles;
            std::error_code iteratorError;
            std::filesystem::recursive_directory_iterator iterator(
                root_, std::filesystem::directory_options::skip_permission_denied, iteratorError);
            const std::filesystem::recursive_directory_iterator end;
            if (iteratorError) {
                throw std::runtime_error("Could not enumerate the project directory: " + iteratorError.message());
            }

            while (iterator != end) {
                const std::filesystem::directory_entry entry = *iterator;
                std::error_code entryError;
                const std::filesystem::path relative = entry.path().lexically_relative(root_).lexically_normal();
                const bool symlink = entry.is_symlink(entryError);
                const bool directory = !entryError && entry.is_directory(entryError);
                if (directory && symlink) {
                    iterator.disable_recursion_pending();
                }

                if (entryError) {
                    result.diagnostics.push_back("Could not inspect '" + relative.generic_string() +
                                                 "': " + entryError.message());
                } else if (!relative.empty() && !isTransientProjectFile(relative)) {
                    const bool protectedEntry = isProtectedProjectEntry(relative, manifest_, projectFile_) || symlink;
                    nextEntries.push_back({relative, directory ? ProjectEntryKind::Directory : ProjectEntryKind::File,
                                           protectedEntry, std::nullopt});
                    if (!directory && !protectedEntry) {
                        const auto type = assetTypeForPath(relative);
                        if (type.has_value() && entry.is_regular_file(entryError) && !entryError &&
                            existingPathInside(root_, entry.path())) {
                            try {
                                const std::string key = relative.generic_string();
                                const std::uintmax_t fileSize = entry.file_size(entryError);
                                if (entryError) {
                                    throw std::runtime_error("Could not inspect asset file '" + key +
                                                             "': " + entryError.message());
                                }
                                const auto writeTime = entry.last_write_time(entryError);
                                if (entryError) {
                                    throw std::runtime_error("Could not inspect asset file '" + key +
                                                             "': " + entryError.message());
                                }
                                AssetFingerprint fingerprint;
                                const auto observed = observedAssetFiles_.find(key);
                                if (!forceHash && observed != observedAssetFiles_.end() &&
                                    observed->second.fileSize == fileSize && observed->second.writeTime == writeTime) {
                                    fingerprint = observed->second.fingerprint;
                                } else {
                                    fingerprint = fingerprintFile(entry.path());
                                }
                                nextObservedAssetFiles.emplace(key,
                                                               ObservedAssetFile{writeTime, fileSize, fingerprint});
                                discovered.push_back({relative, *type, fingerprint, false});
                            } catch (const std::exception& exception) {
                                result.diagnostics.push_back(exception.what());
                            }
                        }
                    }
                }

                iterator.increment(iteratorError);
                if (iteratorError) {
                    result.diagnostics.push_back("Project scan skipped an entry: " + iteratorError.message());
                    iteratorError.clear();
                }
            }

            const std::vector<AssetRecord> previousRecords = assets_.assets();
            AssetRegistry candidate = assets_;
            for (AssetRecord record : previousRecords) {
                record.available = false;
                candidate.addOrReplace(std::move(record));
            }

            const auto serializedRecordChanged = [](const AssetRecord& left, const AssetRecord& right) {
                return left.id != right.id || left.type != right.type ||
                       left.relativePath.lexically_normal() != right.relativePath.lexically_normal() ||
                       left.displayName != right.displayName || left.fingerprint != right.fingerprint;
            };
            bool registryChanged = registryNeedsUpgrade_;

            for (DiscoveredAsset& file : discovered) {
                const AssetRecord* existing = candidate.findByPath(file.relativePath);
                if (existing == nullptr) {
                    continue;
                }
                AssetRecord updated = *existing;
                const AssetRecord original = updated;
                if (updated.fingerprint.valid && updated.fingerprint != file.fingerprint) {
                    ++result.modified;
                }
                updated.type = file.type;
                updated.displayName = file.relativePath.stem().string();
                updated.fingerprint = file.fingerprint;
                updated.available = true;
                registryChanged |= serializedRecordChanged(original, updated);
                candidate.addOrReplace(std::move(updated));
                file.matched = true;
            }

            std::map<std::string, std::vector<AssetId>> missingByFingerprint;
            for (const AssetRecord& record : candidate.assets()) {
                if (!record.available && record.fingerprint.valid) {
                    missingByFingerprint[fingerprintKey(record.type, record.fingerprint)].push_back(record.id);
                }
            }
            std::map<std::string, std::vector<std::size_t>> newByFingerprint;
            for (std::size_t index = 0; index < discovered.size(); ++index) {
                if (!discovered[index].matched) {
                    newByFingerprint[fingerprintKey(discovered[index].type, discovered[index].fingerprint)].push_back(
                        index);
                }
            }

            for (const auto& [key, newIndices] : newByFingerprint) {
                const auto oldIterator = missingByFingerprint.find(key);
                if (oldIterator == missingByFingerprint.end()) {
                    continue;
                }
                const auto& oldIds = oldIterator->second;
                if (oldIds.size() == 1 && newIndices.size() == 1) {
                    DiscoveredAsset& file = discovered[newIndices.front()];
                    const AssetRecord* existing = candidate.find(oldIds.front());
                    if (existing != nullptr) {
                        AssetRecord moved = *existing;
                        moved.type = file.type;
                        moved.relativePath = file.relativePath;
                        moved.displayName = file.relativePath.stem().string();
                        moved.fingerprint = file.fingerprint;
                        moved.available = true;
                        candidate.addOrReplace(std::move(moved));
                        file.matched = true;
                        registryChanged = true;
                        ++result.moved;
                    }
                } else {
                    result.diagnostics.push_back("Asset move could not be resolved uniquely for fingerprint " + key +
                                                 ".");
                }
            }

            for (DiscoveredAsset& file : discovered) {
                if (file.matched) {
                    continue;
                }
                candidate.addOrReplace({generateAssetId(), file.type, file.relativePath,
                                        file.relativePath.stem().string(), file.fingerprint, true});
                file.matched = true;
                registryChanged = true;
                ++result.added;
            }

            for (const AssetRecord& previous : previousRecords) {
                const AssetRecord* current = candidate.find(previous.id);
                if (previous.available && current != nullptr && !current->available) {
                    ++result.missing;
                }
            }

            for (ProjectEntry& entry : nextEntries) {
                if (entry.kind != ProjectEntryKind::File) {
                    continue;
                }
                const AssetRecord* asset = candidate.findByPath(entry.relativePath);
                if (asset != nullptr && asset->available) {
                    entry.asset = asset->id;
                }
            }
            std::ranges::sort(nextEntries, [](const ProjectEntry& left, const ProjectEntry& right) {
                return left.relativePath.generic_string() < right.relativePath.generic_string();
            });

            if (registryChanged) {
                std::string writeError;
                if (!writeJsonAtomic(root_ / ".lumin/AssetRegistry.json", registryJson(candidate), writeError)) {
                    result.error = std::move(writeError);
                    return result;
                }
            }

            assets_ = std::move(candidate);
            projectEntries_ = std::move(nextEntries);
            observedAssetFiles_ = std::move(nextObservedAssetFiles);
            registryNeedsUpgrade_ = false;
            for (const std::string& diagnostic : result.diagnostics) {
                if (std::ranges::find(diagnostics_, diagnostic) == diagnostics_.end()) {
                    diagnostics_.push_back(diagnostic);
                }
            }
        } catch (const std::exception& exception) {
            result.error = exception.what();
        }
        return result;
    }

    bool ProjectSession::renameAsset(const AssetId& asset, std::string_view newName, std::string& error) {
        try {
            const AssetRecord* existing = assets_.find(asset);
            if (existing == nullptr || newName.empty() ||
                std::ranges::any_of(newName, hasInvalidProjectNameCharacter)) {
                throw std::runtime_error("Asset name is invalid.");
            }
            AssetRecord renamed = *existing;
            const std::filesystem::path absoluteCurrent = root_ / renamed.relativePath;
            if (renamed.type == AssetType::Script &&
                std::ranges::any_of(scripts_.scripts(), [&absoluteCurrent](const scripting::ScriptInfo& info) {
                    std::error_code errorCode;
                    return std::filesystem::equivalent(info.source, absoluteCurrent, errorCode) && !errorCode;
                })) {
                throw std::runtime_error("Detach the script before renaming it.");
            }
            const std::filesystem::path nextRelative =
                renamed.relativePath.parent_path() / (std::string{newName} + renamed.relativePath.extension().string());
            const std::filesystem::path current = checkedProjectPath(root_, renamed.relativePath);
            const std::filesystem::path next = checkedProjectPath(root_, nextRelative);
            if (std::filesystem::exists(next)) {
                throw std::runtime_error("An asset with that name already exists.");
            }
            std::filesystem::rename(current, next);
            renamed.relativePath = nextRelative;
            renamed.displayName = std::string{newName};
            assets_.addOrReplace(renamed);
            if (!writeJsonAtomic(root_ / ".lumin/AssetRegistry.json", registryJson(assets_), error)) {
                return false;
            }
            const AssetSyncResult sync = synchronizeProjectFiles();
            if (!sync.succeeded()) {
                error = sync.error;
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    bool ProjectSession::removeAsset(const AssetId& asset, std::string& error) {
        try {
            const AssetRecord* record = assets_.find(asset);
            if (record == nullptr) {
                throw std::runtime_error("Asset does not exist.");
            }
            if (record->type == AssetType::Mesh) {
                if (const auto iterator = loadedMeshes_.find(asset.value); iterator != loadedMeshes_.end()) {
                    for (const scene::ModelHandle model : level_.modelHandles()) {
                        if (std::ranges::any_of(iterator->second, [&](const LoadedMeshPart& part) {
                                return level_.model(model).mesh == part.mesh;
                            })) {
                            throw std::runtime_error("Asset is referenced by the current scene.");
                        }
                    }
                }
            } else if (record->type == AssetType::Script) {
                const std::filesystem::path absolute = root_ / record->relativePath;
                if (std::ranges::any_of(scripts_.scripts(), [&absolute](const scripting::ScriptInfo& info) {
                        std::error_code errorCode;
                        return std::filesystem::equivalent(info.source, absolute, errorCode) && !errorCode;
                    })) {
                    throw std::runtime_error("Asset is attached to an Actor.");
                }
            } else if (record->type == AssetType::Texture) {
                const std::filesystem::path absolute = root_ / record->relativePath;
                for (const scene::ModelHandle model : level_.modelHandles()) {
                    const auto& textures = level_.model(model).material.textures;
                    if (textures.has_value() && (textures->baseColor == absolute || textures->normal == absolute ||
                                                 textures->roughness == absolute)) {
                        throw std::runtime_error("Asset is referenced by a material.");
                    }
                }
            }
            std::filesystem::remove(checkedProjectPath(root_, record->relativePath));
            loadedMeshes_.erase(asset.value);
            assets_.remove(asset);
            if (!writeJsonAtomic(root_ / ".lumin/AssetRegistry.json", registryJson(assets_), error)) {
                return false;
            }
            const AssetSyncResult sync = synchronizeProjectFiles();
            if (!sync.succeeded()) {
                error = sync.error;
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    std::vector<scene::ActorHandle> ProjectSession::createActorsFromMesh(const AssetId& asset,
                                                                         scene::Transform transform) {
        std::vector<scene::ActorHandle> actors;
        const AssetRecord* record = assets_.find(asset);
        const std::vector<LoadedMeshPart>* parts = meshPartsForAsset(asset);
        if (record == nullptr || parts == nullptr) {
            return actors;
        }
        actors.reserve(parts->size());
        for (const LoadedMeshPart& part : *parts) {
            const scene::ActorHandle handle = level_.spawnActor();
            scene::Actor* actor = level_.actor(handle);
            actor->setName(parts->size() == 1 ? record->displayName : record->displayName + " / " + part.name);
            actor->setPersistentId(generateAssetId().value);
            actor->setTransform(transform);
            actor->setMaterial(part.material);
            actor->attachModel(part.mesh, part.material);
            actors.push_back(handle);
        }
        dirty_ = true;
        return actors;
    }

    std::optional<scene::ActorHandle> ProjectSession::createActorFromMesh(const AssetId& asset,
                                                                          scene::Transform transform) {
        const std::vector<scene::ActorHandle> actors = createActorsFromMesh(asset, transform);
        return actors.empty() ? std::nullopt : std::optional{actors.front()};
    }

    scene::ActorHandle ProjectSession::createLightActor(scene::LocalLight light, scene::Transform transform) {
        if (!scene::validateLocalLight(light)) {
            throw std::invalid_argument("Cannot create a light actor with invalid parameters.");
        }
        const bool isPoint = std::holds_alternative<scene::PointLight>(light);
        const scene::ActorHandle handle = level_.spawnActor();
        scene::Actor* actor = level_.actor(handle);
        actor->setName(isPoint ? "Point Light" : "Spot Light");
        actor->setPersistentId(generateAssetId().value);
        actor->setTransform(transform);
        actor->setLocalLight(std::move(light));
        dirty_ = true;
        return handle;
    }

    const std::vector<ProjectSession::LoadedMeshPart>* ProjectSession::meshPartsForAsset(const AssetId& asset) {
        if (auto iterator = loadedMeshes_.find(asset.value); iterator != loadedMeshes_.end()) {
            const bool ready =
                !iterator->second.empty() && std::ranges::all_of(iterator->second, [this](const LoadedMeshPart& part) {
                    return level_.isMeshAlive(part.mesh);
                });
            if (ready) {
                return &iterator->second;
            }
            loadedMeshes_.erase(iterator);
        }

        const AssetRecord* record = assets_.find(asset);
        if (record == nullptr || !record->available || record->type != AssetType::Mesh) {
            return nullptr;
        }

        std::vector<LoadedMeshPart> loaded;
        try {
            assets::ObjModel model = assets::ObjLoader::loadModel(root_ / record->relativePath);
            loaded.reserve(model.parts.size());
            for (assets::ObjMeshPart& sourcePart : model.parts) {
                scene::Material material = importedMaterial(sourcePart.material, root_, diagnostics_);
                const scene::MeshHandle mesh = level_.addMesh(std::move(sourcePart.mesh));
                loaded.push_back({std::move(sourcePart.name), mesh, std::move(material)});
            }
        } catch (const std::exception& exception) {
            // 部分注册失败时必须撤销已加入 Level 的 Mesh，避免无 Actor 引用的资源泄漏到当前会话。
            for (const LoadedMeshPart& part : loaded) {
                static_cast<void>(level_.removeMesh(part.mesh));
            }
            diagnostics_.push_back(exception.what());
            return nullptr;
        }

        auto [iterator, inserted] = loadedMeshes_.emplace(asset.value, std::move(loaded));
        return inserted && !iterator->second.empty() ? &iterator->second : nullptr;
    }

    const ProjectSession::LoadedMeshPart* ProjectSession::meshPartForAsset(const AssetId& asset,
                                                                           std::string_view part) {
        const std::vector<LoadedMeshPart>* parts = meshPartsForAsset(asset);
        if (parts == nullptr || parts->empty()) {
            return nullptr;
        }
        if (part.empty()) {
            return &parts->front();
        }
        const auto iterator = std::ranges::find(*parts, part, &LoadedMeshPart::name);
        return iterator == parts->end() ? nullptr : &*iterator;
    }

    std::optional<scene::MeshHandle> ProjectSession::meshForAsset(const AssetId& asset) {
        const LoadedMeshPart* part = meshPartForAsset(asset, {});
        return part == nullptr ? std::nullopt : std::optional{part->mesh};
    }

    std::optional<ProjectSession::MeshAssetReference> ProjectSession::meshReferenceFor(scene::MeshHandle mesh) const {
        for (const auto& [id, parts] : loadedMeshes_) {
            const auto part = std::ranges::find(parts, mesh, &LoadedMeshPart::mesh);
            if (part != parts.end()) {
                return MeshAssetReference{AssetId{id}, part->name};
            }
        }
        return std::nullopt;
    }

    std::optional<AssetId> ProjectSession::assetForMesh(scene::MeshHandle mesh) const {
        const auto reference = meshReferenceFor(mesh);
        return reference.has_value() ? std::optional{reference->asset} : std::nullopt;
    }

    void ProjectSession::setSettings(ProjectSettings settings) noexcept {
        normalizeProjectSettings(settings);
        if (settings_ != settings) {
            settings_ = std::move(settings);
            dirty_ = true;
        }
    }

    const ProjectSettings& ProjectSession::settings() const noexcept {
        return settings_;
    }

    void ProjectSession::setRenderSettings(ProjectRenderSettings settings) noexcept {
        settings.taaSharpness = std::clamp(settings.taaSharpness, 0.0f, 1.0f);
        if (settings_.render != settings) {
            settings_.render = std::move(settings);
            dirty_ = true;
        }
    }
    const ProjectRenderSettings& ProjectSession::renderSettings() const noexcept {
        return settings_.render;
    }

} // namespace lumin::project

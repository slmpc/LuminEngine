#include "project/ProjectSession.hpp"

#include "assets/ImageLoader.hpp"
#include "assets/ObjLoader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <random>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace lumin::project {
    namespace {
        using Json = nlohmann::json;
        constexpr std::uint32_t formatVersion = 1;

        Json vectorJson(const glm::vec3& value) {
            return Json::array({value.x, value.y, value.z});
        }

        glm::vec3 readVector(const Json& value, const glm::vec3& fallback = {}) {
            if (!value.is_array() || value.size() != 3) {
                return fallback;
            }
            return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
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

        Json registryJson(const AssetRegistry& registry) {
            Json assets = Json::array();
            for (const AssetRecord& asset : registry.assets()) {
                assets.push_back({{"id", asset.id.value},
                                  {"type", typeName(asset.type)},
                                  {"path", asset.relativePath.generic_string()},
                                  {"name", asset.displayName}});
            }
            return {{"formatVersion", formatVersion}, {"assets", std::move(assets)}};
        }

        AssetRegistry parseRegistry(const Json& value, const std::filesystem::path& root) {
            if (value.value("formatVersion", 0U) != formatVersion || !value.contains("assets") ||
                !value["assets"].is_array()) {
                throw std::runtime_error("Unsupported or malformed asset registry.");
            }
            AssetRegistry registry;
            for (const Json& item : value["assets"]) {
                const auto type = parseType(item.value("type", std::string{}));
                AssetRecord record{{item.value("id", std::string{})},
                                   type.value_or(AssetType::Mesh),
                                   item.value("path", std::string{}),
                                   item.value("name", std::string{})};
                if (!record.id.isValid() || !type.has_value()) {
                    throw std::runtime_error("Asset registry contains an invalid asset record.");
                }
                static_cast<void>(checkedProjectPath(root, record.relativePath));
                registry.addOrReplace(std::move(record));
            }
            return registry;
        }

    } // namespace

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
            if (!scripts_.setScriptRoot(nextRoot / "Content/Scripts")) {
                throw std::runtime_error("Could not switch the script root while scripts are active.");
            }
            root_ = nextRoot;
            projectFile_ = root_ / (std::string{name} + ".luminproject");
            manifest_ = ProjectManifest{formatVersion, std::string{name}, "Content", "Scenes/Main.lumin.scene"};
            assets_.clear();
            loadedMeshes_.clear();
            diagnostics_.clear();
            dirty_ = true;
            return save(error);
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
            const AssetRegistry nextRegistry =
                parseRegistry(readJson(checkedProjectPath(nextRoot, ".lumin/AssetRegistry.json")), nextRoot);
            const Json sceneJson = readJson(scenePath);
            if (sceneJson.value("formatVersion", 0U) != formatVersion || !sceneJson.contains("actors") ||
                !sceneJson["actors"].is_array()) {
                throw std::runtime_error("Unsupported or malformed scene document.");
            }

            level_.clear();
            if (!scripts_.setScriptRoot(nextRoot / nextManifest.contentDirectory / "Scripts")) {
                throw std::runtime_error("Could not switch the script root while scripts are active.");
            }
            root_ = nextRoot;
            projectFile_ = nextProjectFile;
            manifest_ = std::move(nextManifest);
            assets_ = nextRegistry;
            loadedMeshes_.clear();
            diagnostics_.clear();

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
            if (const auto settings = sceneJson.find("renderSettings");
                settings != sceneJson.end() && settings->is_object()) {
                renderSettings_.directLighting = settings->value("directLighting", true);
                renderSettings_.shadows = settings->value("shadows", true);
                renderSettings_.rayTracing = settings->value("rayTracing", true);
                renderSettings_.ssao = settings->value("ssao", true);
                renderSettings_.sharc = settings->value("sharc", true);
                renderSettings_.nrd = settings->value("nrd", true);
                renderSettings_.taa = settings->value("taa", true);
                renderSettings_.splitLambda = settings->value("splitLambda", 0.68f);
                renderSettings_.shadowDistance = settings->value("shadowDistance", 200.0f);
                renderSettings_.exposure = settings->value("exposure", 1.0f);
            }
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
                actor->setMaterial(readMaterial(actorJson.value("material", Json::object()), root_));
                const AssetId meshAsset{actorJson.value("mesh", std::string{})};
                if (meshAsset.isValid()) {
                    if (const auto mesh = meshForAsset(meshAsset); mesh.has_value()) {
                        actor->attachModel(*mesh, actor->material());
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
                if (const auto meshAsset = assetForMesh(level_.model(actor->modelHandle()).mesh);
                    meshAsset.has_value()) {
                    actorJson["mesh"] = meshAsset->value;
                }
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
                       {"renderSettings",
                        {{"directLighting", renderSettings_.directLighting},
                         {"shadows", renderSettings_.shadows},
                         {"rayTracing", renderSettings_.rayTracing},
                         {"ssao", renderSettings_.ssao},
                         {"sharc", renderSettings_.sharc},
                         {"nrd", renderSettings_.nrd},
                         {"taa", renderSettings_.taa},
                         {"splitLambda", renderSettings_.splitLambda},
                         {"shadowDistance", renderSettings_.shadowDistance},
                         {"exposure", renderSettings_.exposure}}},
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
        diagnostics_.clear();
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
    const std::vector<std::string>& ProjectSession::diagnostics() const noexcept {
        return diagnostics_;
    }

    std::vector<ImportItemResult> ProjectSession::importAssets(std::span<const ImportRequest> requests) {
        std::vector<ImportItemResult> results;
        results.reserve(requests.size());
        for (const ImportRequest& request : requests) {
            ImportItemResult result{.source = request.source, .asset = std::nullopt, .error = {}};
            std::filesystem::path temporary;
            try {
                if (!hasProject()) {
                    throw std::runtime_error("No project is open.");
                }
                const auto type = assetTypeForPath(request.source);
                if (!type.has_value()) {
                    throw std::runtime_error("Unsupported asset extension.");
                }
                const std::filesystem::path defaultDirectory = *type == AssetType::Mesh      ? "Content/Meshes"
                                                               : *type == AssetType::Texture ? "Content/Textures"
                                                                                             : "Content/Scripts";
                const std::filesystem::path directory =
                    request.destinationDirectory.empty() ? defaultDirectory : request.destinationDirectory;
                std::filesystem::path relative = directory / request.source.filename();
                std::filesystem::path destination = checkedProjectPath(root_, relative);
                if (std::filesystem::exists(destination)) {
                    if (request.conflict == ImportConflictPolicy::Skip) {
                        throw std::runtime_error("Destination already exists (skipped).");
                    }
                    if (request.conflict == ImportConflictPolicy::Rename) {
                        for (std::size_t suffix = 1; std::filesystem::exists(destination); ++suffix) {
                            relative = directory / (request.source.stem().string() + "_" + std::to_string(suffix) +
                                                    request.source.extension().string());
                            destination = checkedProjectPath(root_, relative);
                        }
                    }
                }
                std::filesystem::create_directories(destination.parent_path());
                temporary = destination.string() + ".importing";
                std::filesystem::copy_file(request.source, temporary,
                                           std::filesystem::copy_options::overwrite_existing);
                if (*type == AssetType::Mesh) {
                    static_cast<void>(assets::ObjLoader::load(temporary));
                } else if (*type == AssetType::Texture) {
                    static_cast<void>(assets::ImageLoader::load(temporary));
                } else {
                    const scripting::ScriptResult validation = scripts_.validate(temporary);
                    if (!validation) {
                        throw std::runtime_error(validation.error.has_value() ? validation.error->message
                                                                              : "Lua validation failed.");
                    }
                }
                std::filesystem::copy_file(temporary, destination, std::filesystem::copy_options::overwrite_existing);
                std::filesystem::remove(temporary);
                const AssetRecord* existing = assets_.findByPath(relative);
                AssetRecord record{existing != nullptr ? existing->id : generateAssetId(), *type, relative,
                                   request.source.stem().string()};
                assets_.addOrReplace(record);
                result.asset = std::move(record);
                dirty_ = true;
            } catch (const std::exception& exception) {
                if (!temporary.empty()) {
                    std::error_code ignored;
                    std::filesystem::remove(temporary, ignored);
                }
                result.error = exception.what();
            }
            results.push_back(std::move(result));
        }
        return results;
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
            assets_.addOrReplace(std::move(renamed));
            dirty_ = true;
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
                        if (level_.model(model).mesh == iterator->second) {
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
            dirty_ = true;
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    std::optional<scene::ActorHandle> ProjectSession::createActorFromMesh(const AssetId& asset,
                                                                          scene::Transform transform) {
        const AssetRecord* record = assets_.find(asset);
        const auto mesh = meshForAsset(asset);
        if (record == nullptr || !mesh.has_value()) {
            return std::nullopt;
        }
        const scene::ActorHandle handle = level_.spawnActor();
        scene::Actor* actor = level_.actor(handle);
        actor->setName(record->displayName);
        actor->setPersistentId(generateAssetId().value);
        actor->setTransform(transform);
        actor->attachModel(*mesh);
        dirty_ = true;
        return handle;
    }

    std::optional<scene::MeshHandle> ProjectSession::meshForAsset(const AssetId& asset) {
        if (const auto iterator = loadedMeshes_.find(asset.value);
            iterator != loadedMeshes_.end() && level_.isMeshAlive(iterator->second)) {
            return iterator->second;
        }
        const AssetRecord* record = assets_.find(asset);
        if (record == nullptr || record->type != AssetType::Mesh) {
            return std::nullopt;
        }
        try {
            const scene::MeshHandle mesh = level_.addMesh(assets::ObjLoader::load(root_ / record->relativePath));
            loadedMeshes_[asset.value] = mesh;
            return mesh;
        } catch (const std::exception& exception) {
            diagnostics_.push_back(exception.what());
            return std::nullopt;
        }
    }

    std::optional<AssetId> ProjectSession::assetForMesh(scene::MeshHandle mesh) const {
        for (const auto& [id, handle] : loadedMeshes_) {
            if (handle == mesh) {
                return AssetId{id};
            }
        }
        return std::nullopt;
    }

    void ProjectSession::setRenderSettings(ProjectRenderSettings settings) noexcept {
        renderSettings_ = settings;
    }
    const ProjectRenderSettings& ProjectSession::renderSettings() const noexcept {
        return renderSettings_;
    }

} // namespace lumin::project

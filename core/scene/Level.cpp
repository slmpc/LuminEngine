#include "scene/Level.hpp"
#include "scene/Terrain.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

#include <glm/ext/matrix_transform.hpp>

namespace lumin::scene {
    namespace {

        bool sameTransform(const Transform& left, const Transform& right) noexcept {
            return left.position == right.position && left.rotationDegrees == right.rotationDegrees &&
                   left.scale == right.scale;
        }

        bool sameMaterial(const Material& left, const Material& right) noexcept {
            return left == right;
        }

        bool referencesSameTextureImages(const Material& left, const Material& right) noexcept {
            if (left.textures.has_value() != right.textures.has_value()) {
                return false;
            }
            return !left.textures.has_value() || left.textures->referencesSameImages(*right.textures);
        }

        bool sameDirectionalLight(const DirectionalLight& left, const DirectionalLight& right) noexcept {
            return left.direction == right.direction && left.color == right.color &&
                   left.illuminanceLux == right.illuminanceLux && left.castsShadows == right.castsShadows;
        }

        bool sameAtmosphere(const AtmosphereParameters& left, const AtmosphereParameters& right) noexcept {
            return left.enabled == right.enabled && left.bottomRadiusKm == right.bottomRadiusKm &&
                   left.topRadiusKm == right.topRadiusKm &&
                   left.rayleighScatteringPerKm == right.rayleighScatteringPerKm &&
                   left.rayleighDensityScaleKm == right.rayleighDensityScaleKm &&
                   left.mieScatteringPerKm == right.mieScatteringPerKm &&
                   left.mieAbsorptionPerKm == right.mieAbsorptionPerKm &&
                   left.mieDensityScaleKm == right.mieDensityScaleKm && left.miePhaseG == right.miePhaseG &&
                   left.ozoneAbsorptionPerKm == right.ozoneAbsorptionPerKm &&
                   left.ozoneLayerCenterKm == right.ozoneLayerCenterKm &&
                   left.ozoneLayerHalfWidthKm == right.ozoneLayerHalfWidthKm && left.groundAlbedo == right.groundAlbedo;
        }

        bool sameAtmosphereTransform(const AtmosphereTransform& left, const AtmosphereTransform& right) noexcept {
            return left.kilometersPerWorldUnit == right.kilometersPerWorldUnit &&
                   left.seaLevelWorldY == right.seaLevelWorldY;
        }

        std::uint32_t nextGeneration(std::uint32_t generation) noexcept {
            ++generation;
            return generation == 0 ? 1 : generation;
        }

        struct TerrainShape {
            std::uint32_t resolutionX = 1;
            std::uint32_t resolutionZ = 1;
            float sizeX = 1.0f;
            float sizeZ = 1.0f;
        };

        TerrainShape terrainShape(const TerrainDesc& description) {
            const std::uint32_t requestedResolution =
                description.resolution == 0 ? description.resolutionX : description.resolution;
            const std::uint32_t requestedResolutionZ =
                description.resolution == 0 ? description.resolutionZ : description.resolution;
            const std::uint32_t resolutionX =
                std::max(1U, description.segmentsX == 0 ? requestedResolution : description.segmentsX);
            const std::uint32_t resolutionZ =
                std::max(1U, description.segmentsZ == 0 ? requestedResolutionZ : description.segmentsZ);
            if (resolutionX > 4096U || resolutionZ > 4096U) {
                throw std::length_error("Terrain resolution is too large.");
            }

            const float sizeX = description.width > 0.0f ? description.width : description.sizeX;
            const float sizeZ = description.depth > 0.0f ? description.depth : description.sizeZ;
            return TerrainShape{resolutionX, resolutionZ, std::max(std::abs(sizeX), 0.0001f),
                                std::max(std::abs(sizeZ), 0.0001f)};
        }

        float terrainHeight(const TerrainDesc& description, float x, float z) {
            float height = description.heightFunction ? description.heightFunction(x, z)
                                                      : std::sin(x * 0.35f) * std::cos(z * 0.27f) * 0.35f;
            if (!std::isfinite(height)) {
                height = 0.0f;
            }
            return height * description.heightScale * description.amplitude;
        }

        void recalculateTerrainNormals(assets::Mesh& mesh) {
            for (assets::Vertex& vertex : mesh.vertices) {
                vertex.normal = glm::vec3{0.0f};
            }
            for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
                const std::uint32_t aIndex = mesh.indices[index + 0];
                const std::uint32_t bIndex = mesh.indices[index + 1];
                const std::uint32_t cIndex = mesh.indices[index + 2];
                const glm::vec3 a = mesh.vertices[aIndex].position;
                const glm::vec3 b = mesh.vertices[bIndex].position;
                const glm::vec3 c = mesh.vertices[cIndex].position;
                const glm::vec3 normal = glm::cross(b - a, c - a);
                if (glm::length(normal) <= 0.000001f) {
                    continue;
                }
                mesh.vertices[aIndex].normal += normal;
                mesh.vertices[bIndex].normal += normal;
                mesh.vertices[cIndex].normal += normal;
            }
            for (assets::Vertex& vertex : mesh.vertices) {
                if (glm::length(vertex.normal) > 0.000001f) {
                    vertex.normal = glm::normalize(vertex.normal);
                } else {
                    vertex.normal = glm::vec3{0.0f, 1.0f, 0.0f};
                }
            }
        }

    } // namespace

    Terrain::Terrain(TerrainDesc description) : description_(std::move(description)) {
        const TerrainShape shape = terrainShape(description_);
        resolutionX_ = shape.resolutionX;
        resolutionZ_ = shape.resolutionZ;
        sizeX_ = shape.sizeX;
        sizeZ_ = shape.sizeZ;
        rebuildMesh();
    }

    Terrain::Terrain(std::uint32_t resolutionX, std::uint32_t resolutionZ, float sizeX, float sizeZ,
                     TerrainHeightFunction heightFunction)
        : Terrain(TerrainDesc{.resolutionX = resolutionX,
                              .resolutionZ = resolutionZ,
                              .sizeX = sizeX,
                              .sizeZ = sizeZ,
                              .heightFunction = std::move(heightFunction)}) {
    }

    Terrain Terrain::generate(TerrainDesc description) {
        return Terrain{std::move(description)};
    }

    assets::Mesh Terrain::generateMesh(const TerrainDesc& description) {
        return Terrain{description}.mesh();
    }

    void Terrain::rebuild() {
        const TerrainShape shape = terrainShape(description_);
        resolutionX_ = shape.resolutionX;
        resolutionZ_ = shape.resolutionZ;
        sizeX_ = shape.sizeX;
        sizeZ_ = shape.sizeZ;
        rebuildMesh();
    }

    void Terrain::setHeightFunction(TerrainHeightFunction heightFunction) {
        description_.heightFunction = std::move(heightFunction);
        rebuild();
    }

    void Terrain::setHeightSample(std::uint32_t x, std::uint32_t z, float height) {
        if (x > resolutionX_ || z > resolutionZ_) {
            throw std::out_of_range("Terrain height sample is out of range.");
        }
        if (!std::isfinite(height)) {
            throw std::invalid_argument("Terrain height sample must be finite.");
        }
        const std::size_t index = static_cast<std::size_t>(z) * (resolutionX_ + 1U) + x;
        heights_[index] = height;
        mesh_.vertices[index].position.y = height;
        recalculateTerrainNormals(mesh_);
        ++revision_;
    }

    float Terrain::heightAt(float x, float z) const noexcept {
        if (heights_.empty()) {
            return 0.0f;
        }
        const float clampedX = std::clamp(x, -sizeX_ * 0.5f, sizeX_ * 0.5f);
        const float clampedZ = std::clamp(z, -sizeZ_ * 0.5f, sizeZ_ * 0.5f);
        const float gridX = (clampedX + sizeX_ * 0.5f) / sizeX_ * static_cast<float>(resolutionX_);
        const float gridZ = (clampedZ + sizeZ_ * 0.5f) / sizeZ_ * static_cast<float>(resolutionZ_);
        const std::uint32_t x0 = std::min(resolutionX_, static_cast<std::uint32_t>(std::floor(gridX)));
        const std::uint32_t z0 = std::min(resolutionZ_, static_cast<std::uint32_t>(std::floor(gridZ)));
        const std::uint32_t x1 = std::min(resolutionX_, x0 + 1U);
        const std::uint32_t z1 = std::min(resolutionZ_, z0 + 1U);
        const float tx = x1 == x0 ? 0.0f : gridX - static_cast<float>(x0);
        const float tz = z1 == z0 ? 0.0f : gridZ - static_cast<float>(z0);
        const auto sample = [this](std::uint32_t sampleX, std::uint32_t sampleZ) {
            return heights_[static_cast<std::size_t>(sampleZ) * (resolutionX_ + 1U) + sampleX];
        };
        const float lower = std::lerp(sample(x0, z0), sample(x1, z0), tx);
        const float upper = std::lerp(sample(x0, z1), sample(x1, z1), tx);
        return std::lerp(lower, upper, tz);
    }

    float Terrain::sampleHeight(float x, float z) const noexcept {
        return heightAt(x, z);
    }

    const TerrainDesc& Terrain::description() const noexcept {
        return description_;
    }

    const assets::Mesh& Terrain::mesh() const noexcept {
        return mesh_;
    }

    std::uint32_t Terrain::resolutionX() const noexcept {
        return resolutionX_;
    }

    std::uint32_t Terrain::resolutionZ() const noexcept {
        return resolutionZ_;
    }

    float Terrain::sizeX() const noexcept {
        return sizeX_;
    }

    float Terrain::sizeZ() const noexcept {
        return sizeZ_;
    }

    std::uint64_t Terrain::revision() const noexcept {
        return revision_;
    }

    std::uint64_t Terrain::meshRevision() const noexcept {
        return revision_;
    }

    void Terrain::rebuildMesh() {
        mesh_ = assets::Mesh{};
        mesh_.name = "procedural-terrain";
        const std::size_t rowSize = static_cast<std::size_t>(resolutionX_) + 1U;
        mesh_.vertices.resize(rowSize * (static_cast<std::size_t>(resolutionZ_) + 1U));
        heights_.resize(mesh_.vertices.size());

        for (std::uint32_t z = 0; z <= resolutionZ_; ++z) {
            for (std::uint32_t x = 0; x <= resolutionX_; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(resolutionX_);
                const float v = static_cast<float>(z) / static_cast<float>(resolutionZ_);
                const float worldX = (u - 0.5f) * sizeX_;
                const float worldZ = (v - 0.5f) * sizeZ_;
                const float height = terrainHeight(description_, worldX, worldZ);
                const std::size_t index = static_cast<std::size_t>(z) * rowSize + x;
                heights_[index] = height;
                mesh_.vertices[index] = assets::Vertex{{worldX, height, worldZ}, {0.0f, 1.0f, 0.0f}, {u, v}};
            }
        }

        mesh_.indices.reserve(static_cast<std::size_t>(resolutionX_) * resolutionZ_ * 6U);
        for (std::uint32_t z = 0; z < resolutionZ_; ++z) {
            for (std::uint32_t x = 0; x < resolutionX_; ++x) {
                const std::uint32_t a = z * (resolutionX_ + 1U) + x;
                const std::uint32_t b = (z + 1U) * (resolutionX_ + 1U) + x;
                const std::uint32_t c = (z + 1U) * (resolutionX_ + 1U) + x + 1U;
                const std::uint32_t d = z * (resolutionX_ + 1U) + x + 1U;
                mesh_.indices.insert(mesh_.indices.end(), {a, b, c, c, d, a});
            }
        }
        recalculateTerrainNormals(mesh_);
        ++revision_;
    }

    TerrainActor::TerrainActor(TerrainDesc description, Material material) : terrain_(std::move(description)) {
        setMaterial(material);
    }

    TerrainActor::TerrainActor(Terrain terrain, Material material) : terrain_(std::move(terrain)) {
        setMaterial(material);
    }

    void TerrainActor::onSpawn(Level& levelValue) {
        terrainMesh_ = levelValue.addMesh(terrain_.mesh());
        attachModel(terrainMesh_, material());
        terrainRevision_ = terrain_.revision();
    }

    void TerrainActor::onDestroy(Level& levelValue) {
        const MeshHandle meshHandle = terrainMesh_;
        detachModel();
        levelValue.removeMesh(meshHandle);
        terrainMesh_ = MeshHandle{};
    }

    void TerrainActor::onTick(float) {
        if (terrainRevision_ == terrain_.revision()) {
            return;
        }
        Level* owner = level();
        if (owner != nullptr && terrainMesh_.isValid()) {
            owner->replaceMesh(terrainMesh_, terrain_.mesh());
        }
        terrainRevision_ = terrain_.revision();
    }

    Terrain& TerrainActor::terrain() noexcept {
        return terrain_;
    }

    const Terrain& TerrainActor::terrain() const noexcept {
        return terrain_;
    }

    void TerrainActor::setTerrain(TerrainDesc description) {
        setTerrain(Terrain{std::move(description)});
    }

    void TerrainActor::setTerrain(Terrain terrainValue) {
        terrain_ = std::move(terrainValue);
        terrainRevision_ = terrain_.revision();
        if (Level* owner = level(); owner != nullptr && terrainMesh_.isValid()) {
            owner->replaceMesh(terrainMesh_, terrain_.mesh());
        }
    }

    float TerrainActor::heightAt(float x, float z) const noexcept {
        return terrain_.heightAt(x, z);
    }

    MeshHandle TerrainActor::terrainMeshHandle() const noexcept {
        return terrainMesh_;
    }

    bool MeshHandle::isValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max() && generation != 0;
    }

    glm::mat4 Transform::matrix() const {
        glm::mat4 result{1.0f};
        result = glm::translate(result, position);
        result = glm::rotate(result, glm::radians(rotationDegrees.x), glm::vec3{1.0f, 0.0f, 0.0f});
        result = glm::rotate(result, glm::radians(rotationDegrees.y), glm::vec3{0.0f, 1.0f, 0.0f});
        result = glm::rotate(result, glm::radians(rotationDegrees.z), glm::vec3{0.0f, 0.0f, 1.0f});
        return glm::scale(result, scale);
    }

    glm::vec3 localLightDirection(const Transform& transform) noexcept {
        glm::mat4 rotation{1.0f};
        rotation = glm::rotate(rotation, glm::radians(transform.rotationDegrees.x), glm::vec3{1.0f, 0.0f, 0.0f});
        rotation = glm::rotate(rotation, glm::radians(transform.rotationDegrees.y), glm::vec3{0.0f, 1.0f, 0.0f});
        rotation = glm::rotate(rotation, glm::radians(transform.rotationDegrees.z), glm::vec3{0.0f, 0.0f, 1.0f});
        return glm::normalize(glm::vec3{rotation * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}});
    }

    void ActorComponent::onAttach(Actor&, Level&) {
    }

    void ActorComponent::onDetach(Actor&, Level&) {
    }

    void ActorComponent::tick(Actor&, Level&, float) {
    }

    Actor::~Actor() = default;

    void Actor::onSpawn(Level&) {
    }

    void Actor::onDestroy(Level&) {
    }

    void Actor::tick(Level& levelValue, float deltaSeconds) {
        onTick(levelValue, deltaSeconds);
    }

    void Actor::tick(float deltaSeconds) {
        onTick(deltaSeconds);
    }

    void Actor::onTick(Level&, float deltaSeconds) {
        tick(deltaSeconds);
    }

    void Actor::onTick(float) {
    }

    Level* Actor::level() noexcept {
        return owner_;
    }

    const Level* Actor::level() const noexcept {
        return owner_;
    }

    ActorHandle Actor::handle() const noexcept {
        return handle_;
    }

    bool Actor::isSpawned() const noexcept {
        return owner_ != nullptr && owner_->isActorAlive(handle_);
    }

    bool Actor::isPendingDestroy() const noexcept {
        if (owner_ == nullptr) {
            return false;
        }
        const Level::ActorSlot* slot = owner_->findActorSlot(handle_);
        return slot != nullptr && (slot->state == Level::ActorSlotState::PendingDestroy ||
                                   slot->state == Level::ActorSlotState::PendingSpawnDestroy);
    }

    const std::string& Actor::name() const noexcept {
        return name_;
    }

    void Actor::setName(std::string name) {
        if (name.empty()) {
            name = "Actor";
        }
        if (name_ == name) {
            return;
        }
        name_ = std::move(name);
        if (owner_ != nullptr) {
            owner_->touchActorRevision();
        }
    }

    const std::string& Actor::persistentId() const noexcept {
        return persistentId_;
    }

    void Actor::setPersistentId(std::string id) {
        if (persistentId_ == id) {
            return;
        }
        persistentId_ = std::move(id);
        if (owner_ != nullptr) {
            owner_->touchActorRevision();
        }
    }

    const Transform& Actor::transform() const noexcept {
        return transform_;
    }

    void Actor::setTransform(Transform transform) {
        if (sameTransform(transform_, transform)) {
            return;
        }
        transform_ = transform;
        if (owner_ != nullptr) {
            if (modelHandle_ != InvalidModelHandle || localLight_.has_value()) {
                owner_->updateActorTransform(modelHandle_, transform_, localLight_.has_value());
            } else {
                owner_->touchActorRevision();
            }
        }
    }

    void Actor::translate(const glm::vec3& offset) {
        Transform next = transform_;
        next.position += offset;
        setTransform(next);
    }

    const Material& Actor::material() const noexcept {
        return material_;
    }

    void Actor::setMaterial(Material material) {
        if (sameMaterial(material_, material)) {
            return;
        }
        material_ = material;
        if (owner_ != nullptr) {
            if (modelHandle_ != InvalidModelHandle) {
                owner_->setModelMaterial(modelHandle_, material_);
            } else {
                owner_->touchActorRevision();
            }
        }
    }

    ModelHandle Actor::modelHandle() const noexcept {
        return modelHandle_;
    }

    ModelHandle Actor::attachModel(MeshHandle meshHandle, Material material) {
        if (!meshHandle.isValid()) {
            throw std::invalid_argument("Actor cannot attach an invalid mesh handle.");
        }

        material_ = material;
        requestedMesh_.reset();
        if (owner_ == nullptr) {
            requestedMesh_ = meshHandle;
            return InvalidModelHandle;
        }

        if (modelHandle_ != InvalidModelHandle) {
            owner_->removeModel(modelHandle_);
            modelHandle_ = InvalidModelHandle;
        }
        modelHandle_ = owner_->addModel(meshHandle, transform_, material_);
        return modelHandle_;
    }

    void Actor::detachModel() {
        requestedMesh_.reset();
        if (owner_ != nullptr && modelHandle_ != InvalidModelHandle) {
            owner_->removeModel(modelHandle_);
        }
        modelHandle_ = InvalidModelHandle;
    }

    const std::optional<LocalLight>& Actor::localLight() const noexcept {
        return localLight_;
    }

    void Actor::setLocalLight(LocalLight light) {
        if (!validateLocalLight(light)) {
            throw std::invalid_argument("Actor cannot attach invalid local-light parameters.");
        }
        if (localLight_ == light) {
            return;
        }
        localLight_ = std::move(light);
        if (owner_ != nullptr) {
            owner_->touchRevision(false, false, true);
        }
    }

    void Actor::clearLocalLight() {
        if (!localLight_.has_value()) {
            return;
        }
        localLight_.reset();
        if (owner_ != nullptr) {
            owner_->touchRevision(false, false, true);
        }
    }

    ActorComponent* Actor::addComponent(std::unique_ptr<ActorComponent> componentValue) {
        if (componentValue == nullptr) {
            throw std::invalid_argument("Actor cannot add a null component.");
        }
        ActorComponent* result = componentValue.get();
        components_.push_back(std::move(componentValue));
        if (owner_ != nullptr && owner_->isActorAlive(handle_)) {
            Level* owningLevel = owner_;
            ++owningLevel->callbackDepth_;
            try {
                result->onAttach(*this, *owningLevel);
            } catch (...) {
                --owningLevel->callbackDepth_;
                throw;
            }
            --owningLevel->callbackDepth_;
            owningLevel->touchActorRevision();
            if (!owningLevel->ticking_ && owningLevel->callbackDepth_ == 0 && !owningLevel->flushingActorChanges_ &&
                !owningLevel->destroying_) {
                owningLevel->flushActorChanges();
            }
        }
        return result;
    }

    bool Actor::removeComponent(const ActorComponent* componentValue) {
        const auto iterator = std::find_if(components_.begin(), components_.end(), [componentValue](const auto& value) {
            return value.get() == componentValue;
        });
        if (iterator == components_.end()) {
            return false;
        }
        Level* owningLevel = owner_;
        if (owningLevel != nullptr && owningLevel->isActorAlive(handle_)) {
            ++owningLevel->callbackDepth_;
            try {
                (*iterator)->onDetach(*this, *owningLevel);
            } catch (...) {
                --owningLevel->callbackDepth_;
                throw;
            }
            --owningLevel->callbackDepth_;
        }
        components_.erase(iterator);
        if (owningLevel != nullptr) {
            owningLevel->touchActorRevision();
            if (!owningLevel->ticking_ && owningLevel->callbackDepth_ == 0 && !owningLevel->flushingActorChanges_ &&
                !owningLevel->destroying_) {
                owningLevel->flushActorChanges();
            }
        }
        return true;
    }

    bool Actor::moveComponent(const ActorComponent* componentValue, std::size_t newIndex) {
        if (components_.empty()) {
            return false;
        }
        const auto iterator = std::find_if(components_.begin(), components_.end(), [componentValue](const auto& value) {
            return value.get() == componentValue;
        });
        if (iterator == components_.end()) {
            return false;
        }
        newIndex = std::min(newIndex, components_.size() - 1);
        const std::size_t oldIndex = static_cast<std::size_t>(std::distance(components_.begin(), iterator));
        if (oldIndex == newIndex) {
            return true;
        }
        auto value = std::move(components_[oldIndex]);
        components_.erase(components_.begin() + static_cast<std::ptrdiff_t>(oldIndex));
        components_.insert(components_.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(value));
        if (owner_ != nullptr) {
            owner_->touchActorRevision();
        }
        return true;
    }

    std::size_t Actor::componentCount() const noexcept {
        return components_.size();
    }

    ActorComponent* Actor::component(std::size_t index) noexcept {
        return index < components_.size() ? components_[index].get() : nullptr;
    }

    const ActorComponent* Actor::component(std::size_t index) const noexcept {
        return index < components_.size() ? components_[index].get() : nullptr;
    }

    void Actor::destroy() {
        if (owner_ != nullptr) {
            owner_->destroyActor(handle_);
        }
    }

    Level::~Level() {
        destroying_ = true;
        for (ActorSlot& slot : actors_) {
            if (slot.actor == nullptr) {
                continue;
            }
            if (slot.state == ActorSlotState::PendingSpawn) {
                slot.state = ActorSlotState::PendingSpawnDestroy;
            } else if (slot.state == ActorSlotState::Active) {
                slot.state = ActorSlotState::PendingDestroy;
            }
        }
        flushActorChanges();

        // All callbacks are best effort during shutdown. This final sweep is
        // deliberately not a callback traversal; it only enforces that even an
        // actor spawned from onDestroy cannot retain a dangling Level pointer.
        for (ActorSlot& slot : actors_) {
            if (slot.actor == nullptr) {
                continue;
            }
            if (slot.actor->modelHandle_ != InvalidModelHandle) {
                removeModel(slot.actor->modelHandle_);
            }
            slot.actor->modelHandle_ = InvalidModelHandle;
            slot.actor->owner_ = nullptr;
            slot.actor->handle_ = {};
            slot.actor.reset();
            slot.state = ActorSlotState::Empty;
            slot.generation = nextGeneration(slot.generation);
        }
        destroying_ = false;
    }

    MeshHandle Level::addMesh(assets::Mesh meshValue) {
        if (meshValue.empty()) {
            throw std::invalid_argument("Level cannot add an empty mesh.");
        }

        std::uint32_t index = 0;
        for (; index < meshes_.size(); ++index) {
            if (!meshAlive_[index]) {
                break;
            }
        }
        if (index == meshes_.size()) {
            meshes_.push_back(std::move(meshValue));
            meshGenerations_.push_back(1);
            meshAlive_.push_back(true);
        } else {
            meshes_[index] = std::move(meshValue);
            meshAlive_[index] = true;
            if (meshGenerations_[index] == 0) {
                meshGenerations_[index] = 1;
            }
        }
        touchRevision(true, false);
        return MeshHandle{index, meshGenerations_[index]};
    }

    bool Level::removeMesh(MeshHandle meshHandle) noexcept {
        if (!isMeshAlive(meshHandle)) {
            return false;
        }
        if (std::any_of(models_.begin(), models_.end(), [meshHandle](const ModelInstance& modelValue) {
                return modelValue.mesh == meshHandle;
            })) {
            return false;
        }
        for (const ActorSlot& actorSlot : actors_) {
            if (actorSlot.actor != nullptr && actorSlot.actor->requestedMesh_ == meshHandle) {
                return false;
            }
        }

        meshes_[meshHandle.index] = assets::Mesh{};
        meshAlive_[meshHandle.index] = false;
        meshGenerations_[meshHandle.index] = nextGeneration(meshGenerations_[meshHandle.index]);
        touchRevision(true, false);
        return true;
    }

    bool Level::isMeshAlive(MeshHandle meshHandle) const noexcept {
        return meshHandle.isValid() && meshHandle.index < meshes_.size() && meshAlive_[meshHandle.index] &&
               meshGenerations_[meshHandle.index] == meshHandle.generation;
    }

    ModelHandle Level::addModel(MeshHandle meshHandle, Transform transform, Material material) {
        if (!isMeshAlive(meshHandle)) {
            throw std::out_of_range("Level model references an invalid mesh handle.");
        }

        std::uint32_t slotIndex = 0;
        for (; slotIndex < modelSlots_.size(); ++slotIndex) {
            if (!modelSlots_[slotIndex].active) {
                break;
            }
        }
        if (slotIndex == modelSlots_.size()) {
            modelSlots_.push_back(ModelSlot{.generation = 1});
        }
        ModelSlot& slot = modelSlots_[slotIndex];
        if (slot.generation == 0) {
            slot.generation = 1;
        }
        slot.active = true;
        slot.denseIndex = static_cast<std::uint32_t>(models_.size());
        models_.push_back(ModelInstance{meshHandle, transform, material});
        denseModelSlots_.push_back(slotIndex);
        touchRevision(true, true);
        return ModelHandle{slotIndex, slot.generation};
    }

    bool Level::removeModel(ModelHandle modelHandle) noexcept {
        ModelSlot* slot = findModelSlot(modelHandle);
        if (slot == nullptr) {
            return false;
        }

        const std::uint32_t denseIndex = slot->denseIndex;
        const std::uint32_t lastDenseIndex = static_cast<std::uint32_t>(models_.size() - 1);
        if (denseIndex != lastDenseIndex) {
            models_[denseIndex] = std::move(models_[lastDenseIndex]);
            const std::uint32_t movedSlotIndex = denseModelSlots_[lastDenseIndex];
            denseModelSlots_[denseIndex] = movedSlotIndex;
            modelSlots_[movedSlotIndex].denseIndex = denseIndex;
        }
        models_.pop_back();
        denseModelSlots_.pop_back();

        for (ActorSlot& actorSlot : actors_) {
            if (actorSlot.actor != nullptr && actorSlot.actor->modelHandle_ == modelHandle) {
                actorSlot.actor->modelHandle_ = InvalidModelHandle;
            }
        }
        slot->active = false;
        slot->denseIndex = std::numeric_limits<std::uint32_t>::max();
        slot->generation = nextGeneration(slot->generation);
        touchRevision(true, true);
        return true;
    }

    bool Level::setModelMesh(ModelHandle modelHandle, MeshHandle meshHandle) {
        ModelSlot* slot = findModelSlot(modelHandle);
        if (slot == nullptr) {
            return false;
        }
        if (!isMeshAlive(meshHandle)) {
            throw std::out_of_range("Level model references an invalid mesh handle.");
        }
        ModelInstance& modelValue = models_[slot->denseIndex];
        if (modelValue.mesh == meshHandle) {
            return true;
        }
        modelValue.mesh = meshHandle;
        touchRevision(true, true);
        return true;
    }

    bool Level::setModelTransform(ModelHandle modelHandle, Transform transform) {
        ModelSlot* slot = findModelSlot(modelHandle);
        if (slot == nullptr) {
            return false;
        }
        ModelInstance& modelValue = models_[slot->denseIndex];
        if (sameTransform(modelValue.transform, transform)) {
            return true;
        }
        modelValue.transform = transform;
        touchRevision(false, true);
        return true;
    }

    bool Level::setModelMaterial(ModelHandle modelHandle, Material material) {
        ModelSlot* slot = findModelSlot(modelHandle);
        if (slot == nullptr) {
            return false;
        }
        ModelInstance& modelValue = models_[slot->denseIndex];
        if (sameMaterial(modelValue.material, material)) {
            return true;
        }
        const bool textureSetChanged = !referencesSameTextureImages(modelValue.material, material);
        modelValue.material = material;
        touchRevision(textureSetChanged, true);
        return true;
    }

    bool Level::replaceMesh(MeshHandle meshHandle, assets::Mesh replacement) {
        if (!isMeshAlive(meshHandle)) {
            return false;
        }
        if (replacement.empty()) {
            throw std::invalid_argument("Level cannot replace a mesh with empty geometry.");
        }
        meshes_[meshHandle.index] = std::move(replacement);
        touchRevision(true, false);
        return true;
    }

    const ModelInstance& Level::model(ModelHandle modelHandle) const {
        const ModelSlot* slot = findModelSlot(modelHandle);
        if (slot == nullptr) {
            throw std::out_of_range("Level model handle is invalid.");
        }
        return models_[slot->denseIndex];
    }

    const assets::Mesh& Level::mesh(MeshHandle meshHandle) const {
        if (!isMeshAlive(meshHandle)) {
            throw std::out_of_range("Level mesh handle is invalid.");
        }
        return meshes_[meshHandle.index];
    }

    const std::vector<assets::Mesh>& Level::meshes() const noexcept {
        return meshes_;
    }

    const std::vector<ModelInstance>& Level::models() const noexcept {
        return models_;
    }

    std::vector<ModelHandle> Level::modelHandles() const {
        std::vector<ModelHandle> handles;
        handles.reserve(models_.size());
        for (const std::uint32_t slotIndex : denseModelSlots_) {
            const ModelSlot& slot = modelSlots_[slotIndex];
            handles.push_back(ModelHandle{slotIndex, slot.generation});
        }
        return handles;
    }

    ActorHandle Level::spawnActor(std::unique_ptr<Actor> actorValue) {
        if (actorValue == nullptr) {
            throw std::invalid_argument("Level cannot spawn a null actor.");
        }
        return allocateActorSlot(std::move(actorValue));
    }

    ActorHandle Level::spawnActor() {
        return spawnActor(std::unique_ptr<Actor>{new Actor{}});
    }

    bool Level::destroyActor(ActorHandle handle) {
        ActorSlot* slot = findActorSlot(handle);
        if (slot == nullptr || slot->actor == nullptr || slot->state == ActorSlotState::PendingDestroy ||
            slot->state == ActorSlotState::PendingSpawnDestroy) {
            return false;
        }
        slot->state = slot->state == ActorSlotState::PendingSpawn ? ActorSlotState::PendingSpawnDestroy
                                                                  : ActorSlotState::PendingDestroy;
        if (!ticking_ && callbackDepth_ == 0 && !flushingActorChanges_ && !destroying_) {
            try {
                flushActorChanges();
            } catch (...) {
                const std::exception_ptr error = std::current_exception();
                try {
                    flushActorChanges();
                } catch (...) {
                }
                std::rethrow_exception(error);
            }
        }
        return true;
    }

    bool Level::destroyActor(Actor& actorValue) {
        if (actorValue.owner_ != this) {
            return false;
        }
        ActorSlot* slot = findActorSlot(actorValue.handle_);
        if (slot == nullptr || slot->actor.get() != &actorValue) {
            return false;
        }
        return destroyActor(actorValue.handle());
    }

    bool Level::destroy(ActorHandle handle) {
        return destroyActor(handle);
    }

    bool Level::removeActor(ActorHandle handle) {
        return destroyActor(handle);
    }

    bool Level::isActorAlive(ActorHandle handle) const noexcept {
        const ActorSlot* slot = findActorSlot(handle);
        return slot != nullptr && slot->actor != nullptr && slot->state == ActorSlotState::Active;
    }

    std::vector<ActorHandle> Level::actorHandles() const {
        std::vector<ActorHandle> handles;
        handles.reserve(actorCount());
        for (std::size_t index = 0; index < actors_.size(); ++index) {
            const ActorSlot& slot = actors_[index];
            if (slot.actor != nullptr && slot.state == ActorSlotState::Active) {
                handles.push_back(ActorHandle{static_cast<std::uint32_t>(index), slot.generation});
            }
        }
        return handles;
    }

    Actor* Level::actor(ActorHandle handle) noexcept {
        ActorSlot* slot = findActorSlot(handle);
        if (slot == nullptr || slot->actor == nullptr || slot->state != ActorSlotState::Active) {
            return nullptr;
        }
        return slot->actor.get();
    }

    const Actor* Level::actor(ActorHandle handle) const noexcept {
        const ActorSlot* slot = findActorSlot(handle);
        if (slot == nullptr || slot->actor == nullptr || slot->state != ActorSlotState::Active) {
            return nullptr;
        }
        return slot->actor.get();
    }

    Actor* Level::actorForAttachment(ActorHandle handle) noexcept {
        ActorSlot* slot = findActorSlot(handle);
        if (slot == nullptr || slot->actor == nullptr ||
            (slot->state != ActorSlotState::Active && slot->state != ActorSlotState::PendingSpawn)) {
            return nullptr;
        }
        return slot->actor.get();
    }

    Actor* Level::getActor(ActorHandle handle) noexcept {
        return actor(handle);
    }

    const Actor* Level::getActor(ActorHandle handle) const noexcept {
        return actor(handle);
    }

    std::size_t Level::actorCount() const noexcept {
        std::size_t count = 0;
        for (const ActorSlot& slot : actors_) {
            count += slot.actor != nullptr && slot.state == ActorSlotState::Active ? 1U : 0U;
        }
        return count;
    }

    std::size_t Level::pendingActorCount() const noexcept {
        std::size_t count = 0;
        for (const ActorSlot& slot : actors_) {
            count +=
                slot.actor != nullptr && slot.state != ActorSlotState::Empty && slot.state != ActorSlotState::Active
                    ? 1U
                    : 0U;
        }
        return count;
    }

    std::optional<ActorHandle> Level::actorForModel(ModelHandle modelHandle) const noexcept {
        if (!modelHandle.isValid()) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < actors_.size(); ++index) {
            const ActorSlot& slot = actors_[index];
            if (slot.actor != nullptr && slot.state == ActorSlotState::Active &&
                slot.actor->modelHandle_ == modelHandle) {
                return ActorHandle{static_cast<std::uint32_t>(index), slot.generation};
            }
        }
        return std::nullopt;
    }

    void Level::clear() {
        if (ticking_ || callbackDepth_ != 0 || flushingActorChanges_ || destroying_) {
            throw std::logic_error("Level::clear cannot run during Actor callbacks or tick.");
        }
        const std::vector<ActorHandle> actorSnapshot = actorHandles();
        for (const ActorHandle handle : actorSnapshot) {
            destroyActor(handle);
        }
        const std::vector<ModelHandle> modelSnapshot = modelHandles();
        for (const ModelHandle handle : modelSnapshot) {
            removeModel(handle);
        }
        for (std::size_t index = 0; index < meshAlive_.size(); ++index) {
            if (meshAlive_[index]) {
                removeMesh(MeshHandle{static_cast<std::uint32_t>(index), meshGenerations_[index]});
            }
        }
    }

    void Level::tick(float deltaSeconds) {
        if (ticking_ || callbackDepth_ != 0 || flushingActorChanges_ || destroying_) {
            throw std::logic_error("Level::tick cannot be called recursively.");
        }

        ticking_ = true;
        try {
            const std::size_t initialActorCount = actors_.size();
            for (std::size_t index = 0; index < initialActorCount; ++index) {
                // Re-index after each callback. The callback may append an
                // actor and reallocate the slot vector.
                ActorSlot& slot = actors_[index];
                if (slot.actor != nullptr && slot.state == ActorSlotState::Active) {
                    Actor* actorValue = slot.actor.get();
                    ++callbackDepth_;
                    try {
                        actorValue->tick(*this, deltaSeconds);
                        for (const auto& componentValue : actorValue->components_) {
                            componentValue->tick(*actorValue, *this, deltaSeconds);
                        }
                    } catch (...) {
                        --callbackDepth_;
                        throw;
                    }
                    --callbackDepth_;
                }
            }
            flushActorChanges();
            ticking_ = false;
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            ticking_ = false;
            try {
                flushActorChanges();
            } catch (...) {
            }
            std::rethrow_exception(error);
        }
    }

    std::uint64_t Level::revision() const noexcept {
        return revision_;
    }

    std::uint64_t Level::dataRevision() const noexcept {
        return revision_;
    }

    std::uint64_t Level::topologyRevision() const noexcept {
        return topologyRevision_;
    }

    std::uint64_t Level::modelRevision() const noexcept {
        return modelRevision_;
    }

    std::uint64_t Level::modelsRevision() const noexcept {
        return modelRevision_;
    }

    std::uint64_t Level::meshRevision() const noexcept {
        return topologyRevision_;
    }

    const SceneEnvironment& Level::environment() const noexcept {
        return environment_;
    }

    void Level::setEnvironment(SceneEnvironment environment) noexcept {
        const bool lightingChanged = !sameDirectionalLight(environment_.sun, environment.sun);
        const bool atmosphereChanged =
            !sameAtmosphere(environment_.atmosphere, environment.atmosphere) ||
            !sameAtmosphereTransform(environment_.atmosphereTransform, environment.atmosphereTransform);
        if (!lightingChanged && !atmosphereChanged) {
            return;
        }

        environment_ = std::move(environment);
        ++revision_;
        if (lightingChanged) {
            ++lightingRevision_;
        }
        if (atmosphereChanged) {
            ++atmosphereRevision_;
        }
    }

    void Level::setSun(DirectionalLight sun) noexcept {
        SceneEnvironment next = environment_;
        next.sun = std::move(sun);
        setEnvironment(std::move(next));
    }

    void Level::setAtmosphere(AtmosphereParameters atmosphere) noexcept {
        SceneEnvironment next = environment_;
        next.atmosphere = std::move(atmosphere);
        setEnvironment(std::move(next));
    }

    std::uint64_t Level::lightingRevision() const noexcept {
        return lightingRevision_;
    }

    std::uint64_t Level::atmosphereRevision() const noexcept {
        return atmosphereRevision_;
    }

    Level::ActorSlot* Level::findActorSlot(ActorHandle handle) noexcept {
        if (!handle.isValid() || handle.index >= actors_.size()) {
            return nullptr;
        }
        ActorSlot& slot = actors_[handle.index];
        if (slot.generation != handle.generation || slot.actor == nullptr) {
            return nullptr;
        }
        return &slot;
    }

    const Level::ActorSlot* Level::findActorSlot(ActorHandle handle) const noexcept {
        if (!handle.isValid() || handle.index >= actors_.size()) {
            return nullptr;
        }
        const ActorSlot& slot = actors_[handle.index];
        if (slot.generation != handle.generation || slot.actor == nullptr) {
            return nullptr;
        }
        return &slot;
    }

    Level::ModelSlot* Level::findModelSlot(ModelHandle handle) noexcept {
        if (!handle.isValid() || handle.index >= modelSlots_.size()) {
            return nullptr;
        }
        ModelSlot& slot = modelSlots_[handle.index];
        if (!slot.active || slot.generation != handle.generation || slot.denseIndex >= models_.size()) {
            return nullptr;
        }
        return &slot;
    }

    const Level::ModelSlot* Level::findModelSlot(ModelHandle handle) const noexcept {
        if (!handle.isValid() || handle.index >= modelSlots_.size()) {
            return nullptr;
        }
        const ModelSlot& slot = modelSlots_[handle.index];
        if (!slot.active || slot.generation != handle.generation || slot.denseIndex >= models_.size()) {
            return nullptr;
        }
        return &slot;
    }

    ActorHandle Level::allocateActorSlot(std::unique_ptr<Actor> actorValue) {
        std::uint32_t index = 0;
        for (; index < actors_.size(); ++index) {
            if (actors_[index].actor == nullptr) {
                break;
            }
        }
        if (index == actors_.size()) {
            actors_.push_back(ActorSlot{});
        }

        ActorSlot& slot = actors_[index];
        slot.generation = nextGeneration(slot.generation);
        slot.actor = std::move(actorValue);
        slot.state = destroying_ ? ActorSlotState::PendingSpawnDestroy : ActorSlotState::PendingSpawn;
        slot.actor->owner_ = this;
        slot.actor->handle_ = ActorHandle{index, slot.generation};
        touchRevision(false, false, slot.actor->localLight_.has_value());

        const ActorHandle handle{index, slot.generation};
        if (!ticking_ && callbackDepth_ == 0 && !flushingActorChanges_ && !destroying_) {
            try {
                flushActorChanges();
            } catch (...) {
                const std::exception_ptr error = std::current_exception();
                if (ActorSlot* failedSlot = findActorSlot(handle); failedSlot != nullptr) {
                    failedSlot->state = failedSlot->state == ActorSlotState::PendingSpawn
                                            ? ActorSlotState::PendingSpawnDestroy
                                            : ActorSlotState::PendingDestroy;
                }
                try {
                    flushActorChanges();
                } catch (...) {
                }
                std::rethrow_exception(error);
            }
        }
        return handle;
    }

    void Level::activateActor(ActorHandle handle) {
        ActorSlot* slot = findActorSlot(handle);
        if (slot == nullptr || slot->state != ActorSlotState::PendingSpawn) {
            return;
        }
        Actor* actorValue = slot->actor.get();
        slot->state = ActorSlotState::Active;
        ++callbackDepth_;
        try {
            if (actorValue->requestedMesh_.has_value() && actorValue->modelHandle_ == InvalidModelHandle) {
                const MeshHandle meshHandle = *actorValue->requestedMesh_;
                actorValue->requestedMesh_.reset();
                actorValue->modelHandle_ = addModel(meshHandle, actorValue->transform_, actorValue->material_);
            }
            actorValue->onSpawn(*this);
            for (const auto& componentValue : actorValue->components_) {
                componentValue->onAttach(*actorValue, *this);
            }
        } catch (...) {
            --callbackDepth_;
            if (ActorSlot* currentSlot = findActorSlot(handle);
                currentSlot != nullptr && currentSlot->state == ActorSlotState::Active) {
                currentSlot->state = ActorSlotState::PendingDestroy;
            }
            throw;
        }
        --callbackDepth_;
    }

    void Level::flushActorChanges() {
        if (flushingActorChanges_) {
            return;
        }
        flushingActorChanges_ = true;
        std::exception_ptr firstError;
        bool changed = true;
        try {
            while (changed) {
                changed = false;

                for (std::size_t index = 0; index < actors_.size(); ++index) {
                    const ActorSlot& slot = actors_[index];
                    if (slot.actor != nullptr && (slot.state == ActorSlotState::PendingDestroy ||
                                                  slot.state == ActorSlotState::PendingSpawnDestroy)) {
                        const ActorHandle handle{static_cast<std::uint32_t>(index), slot.generation};
                        try {
                            destroyActorSlot(handle);
                        } catch (...) {
                            if (firstError == nullptr) {
                                firstError = std::current_exception();
                            }
                        }
                        changed = true;
                    }
                }

                for (std::size_t index = 0; index < actors_.size(); ++index) {
                    ActorSlot& slot = actors_[index];
                    if (slot.actor != nullptr && slot.state == ActorSlotState::PendingSpawn) {
                        if (destroying_) {
                            slot.state = ActorSlotState::PendingSpawnDestroy;
                        } else {
                            const ActorHandle handle{static_cast<std::uint32_t>(index), slot.generation};
                            try {
                                activateActor(handle);
                            } catch (...) {
                                if (firstError == nullptr) {
                                    firstError = std::current_exception();
                                }
                            }
                        }
                        changed = true;
                    }
                }
            }
            flushingActorChanges_ = false;
            if (firstError != nullptr && !destroying_) {
                std::rethrow_exception(firstError);
            }
        } catch (...) {
            flushingActorChanges_ = false;
            throw;
        }
    }

    void Level::destroyActorSlot(ActorHandle handle) {
        ActorSlot* slot = findActorSlot(handle);
        if (slot == nullptr ||
            (slot->state != ActorSlotState::PendingDestroy && slot->state != ActorSlotState::PendingSpawnDestroy)) {
            return;
        }

        Actor* actorValue = slot->actor.get();
        const bool spawned = slot->state == ActorSlotState::PendingDestroy;
        const bool hadLocalLight = actorValue->localLight_.has_value();
        std::exception_ptr callbackError;
        if (spawned) {
            ++callbackDepth_;
            try {
                for (auto iterator = actorValue->components_.rbegin(); iterator != actorValue->components_.rend();
                     ++iterator) {
                    (*iterator)->onDetach(*actorValue, *this);
                }
                actorValue->onDestroy(*this);
            } catch (...) {
                callbackError = std::current_exception();
            }
            --callbackDepth_;
        }
        ActorSlot* currentSlot = findActorSlot(handle);
        if (currentSlot == nullptr || currentSlot->actor.get() != actorValue) {
            return;
        }
        if (actorValue->modelHandle_ != InvalidModelHandle) {
            removeModel(actorValue->modelHandle_);
            actorValue->modelHandle_ = InvalidModelHandle;
        }
        actorValue->owner_ = nullptr;
        actorValue->handle_ = {};
        currentSlot->actor.reset();
        currentSlot->state = ActorSlotState::Empty;
        currentSlot->generation = nextGeneration(currentSlot->generation);
        touchRevision(false, false, hadLocalLight);
        if (callbackError != nullptr && !destroying_) {
            std::rethrow_exception(callbackError);
        }
    }

    void Level::touchRevision(bool topology, bool model, bool lighting) {
        ++revision_;
        if (topology) {
            ++topologyRevision_;
        }
        if (model) {
            ++modelRevision_;
        }
        if (lighting) {
            ++lightingRevision_;
        }
    }

    void Level::touchActorRevision() noexcept {
        ++revision_;
        ++modelRevision_;
    }

    void Level::updateActorTransform(ModelHandle modelHandle, const Transform& transform, bool lighting) {
        bool modelChanged = false;
        if (ModelSlot* slot = findModelSlot(modelHandle); slot != nullptr) {
            models_[slot->denseIndex].transform = transform;
            modelChanged = true;
        }
        touchRevision(false, modelChanged, lighting);
    }

} // namespace lumin::scene

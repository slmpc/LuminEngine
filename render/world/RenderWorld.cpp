#include "render/world/RenderWorld.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace lumin::render::world {
    namespace {

        [[nodiscard]] bool handleLess(scene::MeshHandle left, scene::MeshHandle right) noexcept {
            return left.index < right.index || (left.index == right.index && left.generation < right.generation);
        }

        [[nodiscard]] bool handleLess(scene::ModelHandle left, scene::ModelHandle right) noexcept {
            return left.index < right.index || (left.index == right.index && left.generation < right.generation);
        }

        [[nodiscard]] bool sameFloat(float left, float right) noexcept {
            return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
        }

        [[nodiscard]] bool sameVec2(const glm::vec2& left, const glm::vec2& right) noexcept {
            return sameFloat(left.x, right.x) && sameFloat(left.y, right.y);
        }

        [[nodiscard]] bool sameVec3(const glm::vec3& left, const glm::vec3& right) noexcept {
            return sameFloat(left.x, right.x) && sameFloat(left.y, right.y) && sameFloat(left.z, right.z);
        }

        [[nodiscard]] bool sameVertex(const assets::Vertex& left, const assets::Vertex& right) noexcept {
            return sameVec3(left.position, right.position) && sameVec3(left.normal, right.normal) &&
                   sameVec2(left.texCoord, right.texCoord);
        }

        [[nodiscard]] bool sameMesh(const assets::Mesh& left, const assets::Mesh& right) noexcept {
            return left.name == right.name && left.indices == right.indices &&
                   left.vertices.size() == right.vertices.size() &&
                   std::equal(left.vertices.begin(), left.vertices.end(), right.vertices.begin(), sameVertex);
        }

        [[nodiscard]] bool sameTransform(const scene::Transform& left, const scene::Transform& right) noexcept {
            return sameVec3(left.position, right.position) && sameVec3(left.rotationDegrees, right.rotationDegrees) &&
                   sameVec3(left.scale, right.scale);
        }

        [[nodiscard]] bool sameTextureSet(const scene::PbrTextureSet& left,
                                          const scene::PbrTextureSet& right) noexcept {
            return left.baseColor == right.baseColor && left.normal == right.normal &&
                   left.roughness == right.roughness && left.flipNormalY == right.flipNormalY;
        }

        [[nodiscard]] bool sameMaterial(const scene::Material& left, const scene::Material& right) noexcept {
            if (!sameVec3(left.albedo, right.albedo) || left.surfaceModel != right.surfaceModel ||
                !sameFloat(left.metallicRoughness.roughness, right.metallicRoughness.roughness) ||
                !sameFloat(left.metallicRoughness.metallic, right.metallicRoughness.metallic) ||
                !sameVec3(left.blinnPhong.specularColor, right.blinnPhong.specularColor) ||
                !sameFloat(left.blinnPhong.shininess, right.blinnPhong.shininess) ||
                !sameFloat(left.blinnPhong.indexOfRefraction, right.blinnPhong.indexOfRefraction) ||
                !sameFloat(left.textureScale, right.textureScale) ||
                left.textures.has_value() != right.textures.has_value()) {
                return false;
            }
            return !left.textures.has_value() || sameTextureSet(*left.textures, *right.textures);
        }

        [[nodiscard]] bool sameMaterialBinding(const scene::Material& left, const scene::Material& right) noexcept {
            if (left.textures.has_value() != right.textures.has_value()) {
                return false;
            }
            return !left.textures.has_value() || left.textures->referencesSameImages(*right.textures);
        }

        [[nodiscard]] bool sameDirectionalLight(const scene::DirectionalLight& left,
                                                const scene::DirectionalLight& right) noexcept {
            return sameVec3(left.direction, right.direction) && sameVec3(left.color, right.color) &&
                   sameFloat(left.illuminanceLux, right.illuminanceLux) && left.castsShadows == right.castsShadows;
        }

        [[nodiscard]] bool sameAtmosphere(const scene::AtmosphereParameters& left,
                                          const scene::AtmosphereParameters& right) noexcept {
            return left.enabled == right.enabled && sameFloat(left.bottomRadiusKm, right.bottomRadiusKm) &&
                   sameFloat(left.topRadiusKm, right.topRadiusKm) &&
                   sameVec3(left.rayleighScatteringPerKm, right.rayleighScatteringPerKm) &&
                   sameFloat(left.rayleighDensityScaleKm, right.rayleighDensityScaleKm) &&
                   sameVec3(left.mieScatteringPerKm, right.mieScatteringPerKm) &&
                   sameVec3(left.mieAbsorptionPerKm, right.mieAbsorptionPerKm) &&
                   sameFloat(left.mieDensityScaleKm, right.mieDensityScaleKm) &&
                   sameFloat(left.miePhaseG, right.miePhaseG) &&
                   sameVec3(left.ozoneAbsorptionPerKm, right.ozoneAbsorptionPerKm) &&
                   sameFloat(left.ozoneLayerCenterKm, right.ozoneLayerCenterKm) &&
                   sameFloat(left.ozoneLayerHalfWidthKm, right.ozoneLayerHalfWidthKm) &&
                   sameVec3(left.groundAlbedo, right.groundAlbedo);
        }

        [[nodiscard]] bool sameAtmosphereTransform(const scene::AtmosphereTransform& left,
                                                   const scene::AtmosphereTransform& right) noexcept {
            return sameFloat(left.kilometersPerWorldUnit, right.kilometersPerWorldUnit) &&
                   sameFloat(left.seaLevelWorldY, right.seaLevelWorldY);
        }

        [[nodiscard]] bool sameGeometry(const RenderWorldSnapshot& left, const RenderWorldSnapshot& right) noexcept {
            const auto& leftMeshes = left.meshes();
            const auto& rightMeshes = right.meshes();
            if (leftMeshes.size() != rightMeshes.size()) {
                return false;
            }
            for (std::size_t index = 0; index < leftMeshes.size(); ++index) {
                if (leftMeshes[index].sourceHandle != rightMeshes[index].sourceHandle ||
                    !sameMesh(leftMeshes[index].mesh, rightMeshes[index].mesh)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool sameInstanceTopology(const RenderWorldSnapshot& left,
                                                const RenderWorldSnapshot& right) noexcept {
            const auto& leftInstances = left.instances();
            const auto& rightInstances = right.instances();
            if (leftInstances.size() != rightInstances.size()) {
                return false;
            }
            for (std::size_t index = 0; index < leftInstances.size(); ++index) {
                if (leftInstances[index].modelHandle != rightInstances[index].modelHandle ||
                    leftInstances[index].meshIndex != rightInstances[index].meshIndex ||
                    leftInstances[index].model.mesh != rightInstances[index].model.mesh) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] SceneChangeMask sharedInstanceDataChanges(const RenderWorldSnapshot& left,
                                                                const RenderWorldSnapshot& right) noexcept {
            const auto& leftInstances = left.instances();
            const auto& rightInstances = right.instances();
            std::size_t leftIndex = 0;
            std::size_t rightIndex = 0;
            SceneChangeMask changes = SceneChangeMask::None;

            // 两个数组都按稳定 ModelHandle 排序，只比较两个快照中都存在的实例。
            while (leftIndex < leftInstances.size() && rightIndex < rightInstances.size()) {
                const RenderWorldInstance& leftInstance = leftInstances[leftIndex];
                const RenderWorldInstance& rightInstance = rightInstances[rightIndex];
                if (handleLess(leftInstance.modelHandle, rightInstance.modelHandle)) {
                    ++leftIndex;
                    continue;
                }
                if (handleLess(rightInstance.modelHandle, leftInstance.modelHandle)) {
                    ++rightIndex;
                    continue;
                }
                if (!sameTransform(leftInstance.model.transform, rightInstance.model.transform) ||
                    !sameMaterial(leftInstance.model.material, rightInstance.model.material)) {
                    changes |= SceneChangeMask::TransformOrMaterial;
                }
                if (!sameMaterialBinding(leftInstance.model.material, rightInstance.model.material)) {
                    changes |= SceneChangeMask::MaterialBinding;
                }
                ++leftIndex;
                ++rightIndex;
            }
            return changes;
        }

        [[nodiscard]] SceneChangeMask compareSnapshots(const RenderWorldSnapshot& previous,
                                                       const RenderWorldSnapshot& current) noexcept {
            SceneChangeMask changes = SceneChangeMask::None;
            if (!sameGeometry(previous, current)) {
                changes |= SceneChangeMask::Geometry;
            }
            if (!sameInstanceTopology(previous, current)) {
                changes |= SceneChangeMask::InstanceTopology;
            }
            changes |= sharedInstanceDataChanges(previous, current);
            if (!sameDirectionalLight(previous.environment().sun, current.environment().sun)) {
                changes |= SceneChangeMask::Lighting;
            }
            if (!sameAtmosphere(previous.environment().atmosphere, current.environment().atmosphere) ||
                !sameAtmosphereTransform(previous.environment().atmosphereTransform,
                                         current.environment().atmosphereTransform)) {
                changes |= SceneChangeMask::Atmosphere;
            }
            return changes;
        }

        struct SourceModel {
            scene::ModelHandle handle;
            scene::ModelInstance model;
        };

        [[nodiscard]] std::vector<SourceModel> captureSourceModels(const scene::Level& level) {
            std::vector<SourceModel> sourceModels;
            const std::vector<scene::ModelHandle> modelHandles = level.modelHandles();
            sourceModels.reserve(modelHandles.size());
            for (const scene::ModelHandle handle : modelHandles) {
                sourceModels.push_back(SourceModel{handle, level.model(handle)});
            }
            std::sort(sourceModels.begin(), sourceModels.end(), [](const SourceModel& left, const SourceModel& right) {
                return handleLess(left.handle, right.handle);
            });
            return sourceModels;
        }

        [[nodiscard]] std::vector<scene::MeshHandle> collectMeshHandles(const std::vector<SourceModel>& sourceModels) {
            std::vector<scene::MeshHandle> meshHandles;
            meshHandles.reserve(sourceModels.size());
            for (const SourceModel& sourceModel : sourceModels) {
                meshHandles.push_back(sourceModel.model.mesh);
            }
            std::sort(meshHandles.begin(), meshHandles.end(), [](scene::MeshHandle left, scene::MeshHandle right) {
                return handleLess(left, right);
            });
            meshHandles.erase(std::unique(meshHandles.begin(), meshHandles.end()), meshHandles.end());
            return meshHandles;
        }

        [[nodiscard]] std::vector<RenderWorldMesh> captureMeshes(const scene::Level& level,
                                                                 const std::vector<scene::MeshHandle>& meshHandles) {
            std::vector<RenderWorldMesh> meshes;
            meshes.reserve(meshHandles.size());
            for (const scene::MeshHandle handle : meshHandles) {
                meshes.push_back(RenderWorldMesh{handle, level.mesh(handle)});
            }
            return meshes;
        }

        [[nodiscard]] bool sourceGeometryMatches(const scene::Level& level,
                                                 const std::vector<scene::MeshHandle>& sourceHandles,
                                                 const std::vector<RenderWorldMesh>& snapshotMeshes) {
            if (sourceHandles.size() != snapshotMeshes.size()) {
                return false;
            }
            for (std::size_t index = 0; index < sourceHandles.size(); ++index) {
                if (sourceHandles[index] != snapshotMeshes[index].sourceHandle ||
                    !sameMesh(level.mesh(sourceHandles[index]), snapshotMeshes[index].mesh)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::vector<RenderWorldInstance> captureInstances(std::vector<SourceModel> sourceModels,
                                                                        const std::vector<RenderWorldMesh>& meshes) {
            std::vector<RenderWorldInstance> instances;
            instances.reserve(sourceModels.size());
            for (SourceModel& sourceModel : sourceModels) {
                const auto meshPosition = std::lower_bound(meshes.begin(), meshes.end(), sourceModel.model.mesh,
                                                           [](const RenderWorldMesh& left, scene::MeshHandle right) {
                                                               return handleLess(left.sourceHandle, right);
                                                           });
                if (meshPosition == meshes.end() || meshPosition->sourceHandle != sourceModel.model.mesh) {
                    throw std::logic_error("Render world extraction lost a referenced mesh.");
                }
                const auto meshIndex = static_cast<RenderMeshIndex>(std::distance(meshes.begin(), meshPosition));
                instances.push_back(RenderWorldInstance{sourceModel.handle, meshIndex, std::move(sourceModel.model)});
            }
            return instances;
        }

    } // namespace

    RenderWorldSnapshot::RenderWorldSnapshot(std::uint64_t sourceRevision, std::uint64_t sourceTopologyRevision,
                                             std::uint64_t sourceModelRevision, std::uint64_t sourceLightingRevision,
                                             std::uint64_t sourceAtmosphereRevision,
                                             scene::SceneEnvironment environment, std::vector<RenderWorldMesh> meshes,
                                             std::vector<RenderWorldInstance> instances)
        : RenderWorldSnapshot(sourceRevision, sourceTopologyRevision, sourceModelRevision, sourceLightingRevision,
                              sourceAtmosphereRevision, std::move(environment),
                              std::make_shared<const std::vector<RenderWorldMesh>>(std::move(meshes)),
                              std::make_shared<const std::vector<RenderWorldInstance>>(std::move(instances))) {
    }

    RenderWorldSnapshot::RenderWorldSnapshot(std::uint64_t sourceRevision, std::uint64_t sourceTopologyRevision,
                                             std::uint64_t sourceModelRevision, std::uint64_t sourceLightingRevision,
                                             std::uint64_t sourceAtmosphereRevision,
                                             scene::SceneEnvironment environment,
                                             std::shared_ptr<const std::vector<RenderWorldMesh>> meshes,
                                             std::shared_ptr<const std::vector<RenderWorldInstance>> instances)
        : sourceRevision_(sourceRevision), sourceTopologyRevision_(sourceTopologyRevision),
          sourceModelRevision_(sourceModelRevision), sourceLightingRevision_(sourceLightingRevision),
          sourceAtmosphereRevision_(sourceAtmosphereRevision), environment_(std::move(environment)),
          meshes_(std::move(meshes)), instances_(std::move(instances)) {
    }

    std::uint64_t RenderWorldSnapshot::sourceRevision() const noexcept {
        return sourceRevision_;
    }

    std::uint64_t RenderWorldSnapshot::sourceTopologyRevision() const noexcept {
        return sourceTopologyRevision_;
    }

    std::uint64_t RenderWorldSnapshot::sourceModelRevision() const noexcept {
        return sourceModelRevision_;
    }

    std::uint64_t RenderWorldSnapshot::sourceLightingRevision() const noexcept {
        return sourceLightingRevision_;
    }

    std::uint64_t RenderWorldSnapshot::sourceAtmosphereRevision() const noexcept {
        return sourceAtmosphereRevision_;
    }

    const scene::SceneEnvironment& RenderWorldSnapshot::environment() const noexcept {
        return environment_;
    }

    const std::vector<RenderWorldMesh>& RenderWorldSnapshot::meshes() const noexcept {
        return *meshes_;
    }

    const std::vector<RenderWorldInstance>& RenderWorldSnapshot::instances() const noexcept {
        return *instances_;
    }

    const RenderWorldInstance* RenderWorldSnapshot::findInstance(scene::ModelHandle handle) const noexcept {
        const auto found = std::lower_bound(instances_->begin(), instances_->end(), handle,
                                            [](const RenderWorldInstance& instance, scene::ModelHandle candidate) {
                                                return handleLess(instance.modelHandle, candidate);
                                            });
        return found != instances_->end() && found->modelHandle == handle ? &*found : nullptr;
    }

    SceneChangeMask changesBetween(const RenderWorldSnapshotPtr& previous, const RenderWorldSnapshotPtr& current) {
        if (current == nullptr) {
            throw std::invalid_argument("Cannot compare against an empty render-world snapshot.");
        }
        if (previous == nullptr) {
            return SceneChangeMask::All;
        }
        if (previous == current) {
            return SceneChangeMask::None;
        }
        return compareSnapshots(*previous, *current);
    }

    bool SceneDelta::has(SceneChangeMask categories) const noexcept {
        return hasAnyChange(changes, categories);
    }

    bool SceneDelta::changed() const noexcept {
        return changes != SceneChangeMask::None;
    }

    RenderWorldSnapshotPtr RenderWorldExtractor::extract(const scene::Level& level) {
        std::vector<SourceModel> sourceModels = captureSourceModels(level);
        const std::vector<scene::MeshHandle> meshHandles = collectMeshHandles(sourceModels);
        std::vector<RenderWorldMesh> meshes = captureMeshes(level, meshHandles);
        std::vector<RenderWorldInstance> instances = captureInstances(std::move(sourceModels), meshes);

        return RenderWorldSnapshotPtr{new RenderWorldSnapshot(
            level.revision(), level.topologyRevision(), level.modelRevision(), level.lightingRevision(),
            level.atmosphereRevision(), level.environment(), std::move(meshes), std::move(instances))};
    }

    SceneDelta RenderWorldCache::sync(const scene::Level& level) {
        if (snapshot_ == nullptr || sourceLevel_ != &level) {
            snapshot_ = RenderWorldExtractor::extract(level);
            sourceLevel_ = &level;
            return SceneDelta{SceneChangeMask::All, snapshot_};
        }

        if (snapshot_->sourceRevision() == level.revision() &&
            snapshot_->sourceTopologyRevision() == level.topologyRevision() &&
            snapshot_->sourceModelRevision() == level.modelRevision() &&
            snapshot_->sourceLightingRevision() == level.lightingRevision() &&
            snapshot_->sourceAtmosphereRevision() == level.atmosphereRevision()) {
            return SceneDelta{SceneChangeMask::None, snapshot_};
        }

        const std::uint64_t sourceRevision = level.revision();
        const std::uint64_t sourceTopologyRevision = level.topologyRevision();
        const std::uint64_t sourceModelRevision = level.modelRevision();
        const std::uint64_t sourceLightingRevision = level.lightingRevision();
        const std::uint64_t sourceAtmosphereRevision = level.atmosphereRevision();
        RenderWorldSnapshotPtr next;

        if (snapshot_->sourceTopologyRevision() == sourceTopologyRevision &&
            snapshot_->sourceModelRevision() == sourceModelRevision) {
            next = RenderWorldSnapshotPtr{new RenderWorldSnapshot(
                sourceRevision, sourceTopologyRevision, sourceModelRevision, sourceLightingRevision,
                sourceAtmosphereRevision, level.environment(), snapshot_->meshes_, snapshot_->instances_)};
        } else {
            std::vector<SourceModel> sourceModels = captureSourceModels(level);
            std::shared_ptr<const std::vector<RenderWorldMesh>> meshes = snapshot_->meshes_;

            if (snapshot_->sourceTopologyRevision() != sourceTopologyRevision) {
                const std::vector<scene::MeshHandle> meshHandles = collectMeshHandles(sourceModels);
                if (!sourceGeometryMatches(level, meshHandles, *meshes)) {
                    meshes = std::make_shared<const std::vector<RenderWorldMesh>>(captureMeshes(level, meshHandles));
                }
            }

            std::vector<RenderWorldInstance> instances = captureInstances(std::move(sourceModels), *meshes);
            next = RenderWorldSnapshotPtr{new RenderWorldSnapshot(
                sourceRevision, sourceTopologyRevision, sourceModelRevision, sourceLightingRevision,
                sourceAtmosphereRevision, level.environment(), std::move(meshes),
                std::make_shared<const std::vector<RenderWorldInstance>>(std::move(instances)))};
        }

        const SceneChangeMask changes = changesBetween(snapshot_, next);
        snapshot_ = std::move(next);
        return SceneDelta{changes, snapshot_};
    }

    RenderWorldSnapshotPtr RenderWorldCache::snapshot() const noexcept {
        return snapshot_;
    }

    void RenderWorldCache::clear() noexcept {
        snapshot_.reset();
        sourceLevel_ = nullptr;
    }

} // namespace lumin::render::world

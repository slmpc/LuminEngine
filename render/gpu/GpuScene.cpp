#include "render/gpu/GpuScene.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lumin::render::gpu {
    namespace {

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

        [[nodiscard]] bool sameGpuMesh(const assets::Mesh& left, const assets::Mesh& right) noexcept {
            return left.indices == right.indices && left.vertices.size() == right.vertices.size() &&
                   std::equal(left.vertices.begin(), left.vertices.end(), right.vertices.begin(), sameVertex);
        }

        [[nodiscard]] bool hasSameVertexPositions(const assets::Mesh& left, const assets::Mesh& right) noexcept {
            if (left.vertices.size() != right.vertices.size()) {
                return false;
            }
            for (std::size_t index = 0; index < left.vertices.size(); ++index) {
                if (!sameVec3(left.vertices[index].position, right.vertices[index].position)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool hasRefitCompatibleTopology(const assets::Mesh& left, const assets::Mesh& right) noexcept {
            return left.vertices.size() == right.vertices.size() && left.indices == right.indices;
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

        template <typename Index> [[nodiscard]] Index allocateIndex(std::uint32_t& nextIndex) {
            if (nextIndex == Index::invalidValue) {
                throw std::overflow_error("GPU scene stable index space is exhausted.");
            }
            const Index result{nextIndex};
            ++nextIndex;
            return result;
        }

        [[nodiscard]] const GpuMeshBinding* findMeshBinding(const std::vector<GpuMeshBinding>& bindings,
                                                            scene::MeshHandle handle) noexcept {
            const auto found = std::ranges::find_if(bindings, [handle](const GpuMeshBinding& binding) {
                return binding.sourceHandle == handle;
            });
            return found == bindings.end() ? nullptr : &*found;
        }

        [[nodiscard]] const GpuInstanceBinding* findInstanceBinding(const std::vector<GpuInstanceBinding>& bindings,
                                                                    RenderInstanceId id) noexcept {
            const auto found = std::ranges::find_if(bindings, [id](const GpuInstanceBinding& binding) {
                return binding.instanceId == id;
            });
            return found == bindings.end() ? nullptr : &*found;
        }

        [[nodiscard]] const world::RenderWorldMesh* previousMesh(const world::RenderWorldSnapshotPtr& snapshot,
                                                                 const GpuSceneLayout& layout,
                                                                 scene::MeshHandle handle) {
            if (!snapshot) {
                return nullptr;
            }
            const GpuMeshBinding* binding = layout.findMesh(handle);
            if (binding == nullptr) {
                return nullptr;
            }
            if (binding->snapshotMeshIndex >= snapshot->meshes().size()) {
                throw std::logic_error("Committed GPU mesh layout does not match its immutable snapshot.");
            }
            return &snapshot->meshes()[binding->snapshotMeshIndex];
        }

        [[nodiscard]] const world::RenderWorldInstance* previousInstance(const world::RenderWorldSnapshotPtr& snapshot,
                                                                         const GpuSceneLayout& layout,
                                                                         RenderInstanceId id) {
            if (!snapshot) {
                return nullptr;
            }
            const GpuInstanceBinding* binding = layout.findInstance(id);
            if (binding == nullptr) {
                return nullptr;
            }
            if (binding->snapshotInstanceIndex >= snapshot->instances().size()) {
                throw std::logic_error("Committed GPU instance layout does not match its immutable snapshot.");
            }
            return &snapshot->instances()[binding->snapshotInstanceIndex];
        }

        [[nodiscard]] bool sameInstanceTopology(const GpuSceneLayout& previous, const GpuSceneLayout& target) noexcept {
            if (previous.instances().size() != target.instances().size()) {
                return false;
            }
            return std::ranges::all_of(target.instances(), [&previous](const GpuInstanceBinding& targetBinding) {
                const GpuInstanceBinding* previousBinding = previous.findInstance(targetBinding.instanceId);
                return previousBinding != nullptr && previousBinding->instanceIndex == targetBinding.instanceIndex &&
                       previousBinding->meshIndex == targetBinding.meshIndex &&
                       previousBinding->materialIndex == targetBinding.materialIndex;
            });
        }

        [[nodiscard]] bool hasBlasWork(std::span<const BlasUpdateDecision> decisions) noexcept {
            return std::ranges::any_of(decisions, [](const BlasUpdateDecision& decision) {
                return decision.mode != BlasUpdateMode::Reuse;
            });
        }

    } // namespace

    float materialDenoisingRoughness(const scene::Material& material) noexcept {
        if (material.surfaceModel == scene::SurfaceModel::BlinnPhong) {
            const float shininess = std::isfinite(material.blinnPhong.shininess)
                                        ? std::clamp(material.blinnPhong.shininess, 1.0f, 8192.0f)
                                        : 48.0f;
            return std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.02f, 1.0f);
        }
        const float roughness =
            std::isfinite(material.metallicRoughness.roughness) ? material.metallicRoughness.roughness : 0.45f;
        return std::clamp(roughness, 0.02f, 1.0f);
    }

    GpuMaterialData packGpuMaterial(const scene::Material& material, std::uint32_t textureDescriptorIndex) noexcept {
        const auto finiteColor = [](const glm::vec3& color, const glm::vec3& fallback) noexcept {
            glm::vec3 result;
            for (glm::length_t index = 0; index < 3; ++index) {
                result[index] = std::isfinite(color[index]) ? std::clamp(color[index], 0.0f, 1.0f) : fallback[index];
            }
            return result;
        };
        const glm::vec3 baseColor = finiteColor(material.albedo, glm::vec3{0.82f, 0.68f, 0.48f});
        const glm::vec3 specularColor = finiteColor(material.blinnPhong.specularColor, glm::vec3{0.04f});
        const float metallic = material.surfaceModel == scene::SurfaceModel::MetallicRoughness &&
                                       std::isfinite(material.metallicRoughness.metallic)
                                   ? std::clamp(material.metallicRoughness.metallic, 0.0f, 1.0f)
                                   : 0.0f;
        const float shininess = std::isfinite(material.blinnPhong.shininess)
                                    ? std::clamp(material.blinnPhong.shininess, 1.0f, 8192.0f)
                                    : 48.0f;
        const float textureScale =
            std::isfinite(material.textureScale) ? std::max(std::abs(material.textureScale), 0.0001f) : 1.0f;
        const bool hasTextures = material.textures.has_value() && material.textures->complete();
        const float normalYSign = hasTextures && material.textures->flipNormalY ? -1.0f : 1.0f;

        return GpuMaterialData{
            .baseColorMetallic = glm::vec4{baseColor, metallic},
            .specularColorShininess = glm::vec4{specularColor, shininess},
            .surfaceParameters = glm::vec4{materialDenoisingRoughness(material), textureScale, normalYSign, 0.0f},
            .metadata = glm::uvec4{static_cast<std::uint32_t>(material.surfaceModel),
                                   hasTextures ? textureDescriptorIndex : 0U, hasTextures ? 1U : 0U, 0U},
        };
    }

    const std::vector<GpuMeshBinding>& GpuSceneLayout::meshes() const noexcept {
        return meshes_;
    }

    const std::vector<GpuInstanceBinding>& GpuSceneLayout::instances() const noexcept {
        return instances_;
    }

    const GpuMeshBinding* GpuSceneLayout::findMesh(scene::MeshHandle handle) const noexcept {
        return findMeshBinding(meshes_, handle);
    }

    const GpuInstanceBinding* GpuSceneLayout::findInstance(RenderInstanceId id) const noexcept {
        return findInstanceBinding(instances_, id);
    }

    std::uint64_t GpuSceneUpdatePlan::baseGeneration() const noexcept {
        return baseGeneration_;
    }

    bool GpuSceneUpdatePlan::initializesScene() const noexcept {
        return initializesScene_;
    }

    world::SceneChangeMask GpuSceneUpdatePlan::changes() const noexcept {
        return changes_;
    }

    const world::RenderWorldSnapshotPtr& GpuSceneUpdatePlan::previousSnapshot() const noexcept {
        return previousSnapshot_;
    }

    const world::RenderWorldSnapshotPtr& GpuSceneUpdatePlan::targetSnapshot() const noexcept {
        return targetSnapshot_;
    }

    const GpuSceneLayout& GpuSceneUpdatePlan::targetLayout() const noexcept {
        return targetLayout_;
    }

    std::span<const GpuGeometryUpload> GpuSceneUpdatePlan::geometryUploads() const noexcept {
        return geometryUploads_;
    }

    bool GpuSceneUpdatePlan::rebuildsInstanceTopology() const noexcept {
        return rebuildInstanceTopology_;
    }

    std::span<const GpuInstanceRecord> GpuSceneUpdatePlan::instanceRecords() const noexcept {
        return instanceRecords_;
    }

    std::span<const GpuInstancePatch> GpuSceneUpdatePlan::instancePatches() const noexcept {
        return instancePatches_;
    }

    bool GpuSceneUpdatePlan::rebuildsMaterialBindings() const noexcept {
        return rebuildMaterialBindings_;
    }

    std::span<const GpuLightPatch> GpuSceneUpdatePlan::lightPatches() const noexcept {
        return lightPatches_;
    }

    std::span<const BlasUpdateDecision> GpuSceneUpdatePlan::blasDecisions() const noexcept {
        return blasDecisions_;
    }

    TlasUpdateMode GpuSceneUpdatePlan::tlasDecision() const noexcept {
        return tlasDecision_;
    }

    std::span<const GpuMeshIndex> GpuSceneUpdatePlan::retiredMeshes() const noexcept {
        return retiredMeshes_;
    }

    std::span<const GpuInstanceIndex> GpuSceneUpdatePlan::retiredInstances() const noexcept {
        return retiredInstances_;
    }

    std::span<const GpuMaterialIndex> GpuSceneUpdatePlan::retiredMaterials() const noexcept {
        return retiredMaterials_;
    }

    bool GpuSceneUpdatePlan::hasGpuWork() const noexcept {
        return !geometryUploads_.empty() || rebuildInstanceTopology_ || !instancePatches_.empty() ||
               rebuildMaterialBindings_ || !lightPatches_.empty() || hasBlasWork(blasDecisions_) ||
               tlasDecision_ != TlasUpdateMode::Reuse || !retiredMeshes_.empty() || !retiredInstances_.empty() ||
               !retiredMaterials_.empty();
    }

    bool GpuSceneUpdatePlan::requiresInFlightResourcePreservation() const noexcept {
        return previousSnapshot_ && (!geometryUploads_.empty() || hasBlasWork(blasDecisions_) ||
                                     tlasDecision_ != TlasUpdateMode::Reuse || !retiredMeshes_.empty());
    }

    GpuSceneUpdatePlan GpuSceneUpdatePlanner::plan(const world::SceneDelta& delta) const {
        if (!delta.snapshot) {
            throw std::invalid_argument("GPU scene planning requires a non-null RenderWorld snapshot.");
        }

        GpuSceneUpdatePlan result;
        result.owner_ = this;
        result.ownerEpoch_ = epoch_;
        result.baseGeneration_ = generation_;
        result.initializesScene_ = !snapshot_;
        result.changes_ = result.initializesScene_ ? world::SceneChangeMask::All : delta.changes;
        result.previousSnapshot_ = snapshot_;
        result.targetSnapshot_ = delta.snapshot;
        result.meshIndexRegistry_ = meshIndexRegistry_;
        result.instanceIndexRegistry_ = instanceIndexRegistry_;
        result.nextMeshIndex_ = nextMeshIndex_;
        result.nextInstanceIndex_ = nextInstanceIndex_;

        const bool fullRebuild = result.initializesScene_ || result.changes_ == world::SceneChangeMask::All;

        const auto& targetMeshes = result.targetSnapshot_->meshes();
        result.targetLayout_.meshes_.reserve(targetMeshes.size());
        for (std::size_t snapshotIndex = 0; snapshotIndex < targetMeshes.size(); ++snapshotIndex) {
            if (snapshotIndex > std::numeric_limits<world::RenderMeshIndex>::max()) {
                throw std::overflow_error("RenderWorld mesh count exceeds its compact index type.");
            }
            const scene::MeshHandle handle = targetMeshes[snapshotIndex].sourceHandle;
            const GpuMeshBinding* registered = findMeshBinding(result.meshIndexRegistry_, handle);
            GpuMeshIndex gpuIndex;
            if (registered != nullptr) {
                gpuIndex = registered->gpuIndex;
            } else {
                gpuIndex = allocateIndex<GpuMeshIndex>(result.nextMeshIndex_);
                result.meshIndexRegistry_.push_back(GpuMeshBinding{handle, gpuIndex, 0});
            }
            result.targetLayout_.meshes_.push_back(
                GpuMeshBinding{handle, gpuIndex, static_cast<world::RenderMeshIndex>(snapshotIndex)});
        }

        const auto& targetInstances = result.targetSnapshot_->instances();
        result.targetLayout_.instances_.reserve(targetInstances.size());
        for (std::size_t snapshotIndex = 0; snapshotIndex < targetInstances.size(); ++snapshotIndex) {
            if (snapshotIndex > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("RenderWorld instance count exceeds the GPU scene index type.");
            }
            const world::RenderWorldInstance& sourceInstance = targetInstances[snapshotIndex];
            const RenderInstanceId instanceId{sourceInstance.modelHandle};
            if (!instanceId.isValid()) {
                throw std::logic_error("RenderWorld snapshot contains an invalid model handle.");
            }
            if (sourceInstance.meshIndex >= result.targetLayout_.meshes_.size()) {
                throw std::logic_error("RenderWorld instance references an invalid compact mesh index.");
            }

            const GpuInstanceBinding* registered = findInstanceBinding(result.instanceIndexRegistry_, instanceId);
            GpuInstanceIndex instanceIndex;
            GpuMaterialIndex materialIndex;
            if (registered != nullptr) {
                instanceIndex = registered->instanceIndex;
                materialIndex = registered->materialIndex;
            } else {
                instanceIndex = allocateIndex<GpuInstanceIndex>(result.nextInstanceIndex_);
                materialIndex = materialIndexFor(instanceId);
                result.instanceIndexRegistry_.push_back(
                    GpuInstanceBinding{instanceId, instanceIndex, {}, materialIndex, 0});
            }
            result.targetLayout_.instances_.push_back(GpuInstanceBinding{
                instanceId, instanceIndex, result.targetLayout_.meshes_[sourceInstance.meshIndex].gpuIndex,
                materialIndex, static_cast<std::uint32_t>(snapshotIndex)});
        }

        for (const GpuMeshBinding& previousBinding : layout_.meshes()) {
            if (result.targetLayout_.findMesh(previousBinding.sourceHandle) == nullptr) {
                result.retiredMeshes_.push_back(previousBinding.gpuIndex);
            }
        }
        for (const GpuInstanceBinding& previousBinding : layout_.instances()) {
            if (result.targetLayout_.findInstance(previousBinding.instanceId) == nullptr) {
                result.retiredInstances_.push_back(previousBinding.instanceIndex);
                const bool materialSlotReused =
                    std::ranges::any_of(result.targetLayout_.instances_, [&](const GpuInstanceBinding& binding) {
                        return binding.materialIndex == previousBinding.materialIndex;
                    });
                if (!materialSlotReused) {
                    result.retiredMaterials_.push_back(previousBinding.materialIndex);
                }
            }
        }

        result.blasDecisions_.reserve(result.targetLayout_.meshes_.size());
        for (const GpuMeshBinding& targetBinding : result.targetLayout_.meshes_) {
            const world::RenderWorldMesh& targetMesh = targetMeshes[targetBinding.snapshotMeshIndex];
            const world::RenderWorldMesh* oldMesh = previousMesh(snapshot_, layout_, targetBinding.sourceHandle);
            const bool gpuGeometryChanged =
                fullRebuild || oldMesh == nullptr || !sameGpuMesh(oldMesh->mesh, targetMesh.mesh);
            if (gpuGeometryChanged) {
                const bool requiresResize = oldMesh == nullptr ||
                                            oldMesh->mesh.vertices.size() != targetMesh.mesh.vertices.size() ||
                                            oldMesh->mesh.indices.size() != targetMesh.mesh.indices.size();
                result.geometryUploads_.push_back(GpuGeometryUpload{
                    targetBinding.gpuIndex, targetBinding.snapshotMeshIndex, requiresResize, oldMesh != nullptr});
            }

            BlasUpdateMode blasMode = BlasUpdateMode::Reuse;
            if (fullRebuild || oldMesh == nullptr) {
                blasMode = BlasUpdateMode::Build;
            } else if (!sameGpuMesh(oldMesh->mesh, targetMesh.mesh)) {
                if (oldMesh->mesh.indices == targetMesh.mesh.indices &&
                    hasSameVertexPositions(oldMesh->mesh, targetMesh.mesh)) {
                    blasMode = BlasUpdateMode::Reuse;
                } else if (hasRefitCompatibleTopology(oldMesh->mesh, targetMesh.mesh)) {
                    blasMode = BlasUpdateMode::Refit;
                } else {
                    blasMode = BlasUpdateMode::Build;
                }
            }
            result.blasDecisions_.push_back(
                BlasUpdateDecision{targetBinding.gpuIndex, targetBinding.snapshotMeshIndex, blasMode});
        }

        result.rebuildInstanceTopology_ =
            fullRebuild || world::hasAnyChange(result.changes_, world::SceneChangeMask::InstanceTopology) ||
            !sameInstanceTopology(layout_, result.targetLayout_);
        if (result.rebuildInstanceTopology_) {
            result.instanceRecords_.reserve(result.targetLayout_.instances_.size());
            for (const GpuInstanceBinding& binding : result.targetLayout_.instances_) {
                result.instanceRecords_.push_back(GpuInstanceRecord{binding.instanceId, binding.instanceIndex,
                                                                    binding.meshIndex, binding.materialIndex,
                                                                    binding.snapshotInstanceIndex});
            }
        } else {
            for (const GpuInstanceBinding& binding : result.targetLayout_.instances_) {
                const world::RenderWorldInstance* oldInstance =
                    previousInstance(snapshot_, layout_, binding.instanceId);
                if (oldInstance == nullptr) {
                    throw std::logic_error("Stable GPU instance layout lost its previous snapshot record.");
                }
                const world::RenderWorldInstance& targetInstance = targetInstances[binding.snapshotInstanceIndex];
                GpuInstancePatchMask fields = GpuInstancePatchMask::None;
                if (!sameTransform(oldInstance->model.transform, targetInstance.model.transform)) {
                    fields |= GpuInstancePatchMask::Transform;
                }
                if (!sameMaterial(oldInstance->model.material, targetInstance.model.material)) {
                    fields |= GpuInstancePatchMask::Material;
                }
                if (fields != GpuInstancePatchMask::None) {
                    result.instancePatches_.push_back(GpuInstancePatch{binding.instanceId, binding.instanceIndex,
                                                                       binding.materialIndex,
                                                                       binding.snapshotInstanceIndex, fields});
                }
            }
        }

        bool materialBindingsChanged = result.rebuildInstanceTopology_ ||
                                       world::hasAnyChange(result.changes_, world::SceneChangeMask::MaterialBinding);
        if (!materialBindingsChanged && snapshot_) {
            for (const GpuInstanceBinding& binding : result.targetLayout_.instances_) {
                const world::RenderWorldInstance* oldInstance =
                    previousInstance(snapshot_, layout_, binding.instanceId);
                const world::RenderWorldInstance& targetInstance = targetInstances[binding.snapshotInstanceIndex];
                if (oldInstance == nullptr ||
                    !sameMaterialBinding(oldInstance->model.material, targetInstance.model.material)) {
                    materialBindingsChanged = true;
                    break;
                }
            }
        }
        result.rebuildMaterialBindings_ = fullRebuild || materialBindingsChanged;

        const bool lightChanged =
            fullRebuild || !snapshot_ || world::hasAnyChange(result.changes_, world::SceneChangeMask::Lighting) ||
            !sameDirectionalLight(snapshot_->environment().sun, result.targetSnapshot_->environment().sun);
        if (lightChanged) {
            result.lightPatches_.push_back(GpuLightPatch{sunLightGpuIndex});
        }

        if (result.rebuildInstanceTopology_) {
            result.tlasDecision_ = TlasUpdateMode::Build;
        } else {
            const bool transformChanged =
                std::ranges::any_of(result.instancePatches_, [](const GpuInstancePatch& patch) {
                    return hasAnyPatch(patch.fields, GpuInstancePatchMask::Transform);
                });
            result.tlasDecision_ =
                transformChanged || hasBlasWork(result.blasDecisions_) ? TlasUpdateMode::Update : TlasUpdateMode::Reuse;
        }

        return result;
    }

    void GpuSceneUpdatePlanner::commit(const GpuSceneUpdatePlan& plan, const GpuSceneCommitInfo& info) {
        if (plan.owner_ != this) {
            throw std::invalid_argument("Cannot commit a GPU scene plan created by another planner.");
        }
        if (plan.ownerEpoch_ != epoch_) {
            throw std::logic_error("Cannot commit a GPU scene plan invalidated by planner reset.");
        }
        if (plan.baseGeneration_ != generation_ || plan.previousSnapshot_ != snapshot_) {
            throw std::logic_error("Cannot commit a stale GPU scene update plan.");
        }
        if (!plan.targetSnapshot_) {
            throw std::invalid_argument("Cannot commit a GPU scene plan without a target snapshot.");
        }
        if (plan.hasGpuWork()) {
            if (!info.frameSlot.isValid()) {
                throw std::invalid_argument("GPU scene writes require a valid frame slot.");
            }
            if (!info.frameSlotFenceWaited) {
                throw std::logic_error("GPU scene writes are only allowed after waiting for the frame-slot fence.");
            }
            if (!info.updateCommandsSubmitted) {
                throw std::logic_error("GPU scene state cannot advance before update commands are submitted.");
            }
            if (plan.requiresInFlightResourcePreservation() && !info.inFlightResourcesPreserved) {
                throw std::logic_error(
                    "Shared GPU scene updates must preserve resources referenced by other in-flight frames.");
            }
        }
        const bool advancesGpuGeneration = plan.hasGpuWork();
        if (advancesGpuGeneration && generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("GPU scene planner generation is exhausted.");
        }

        world::RenderWorldSnapshotPtr nextSnapshot = plan.targetSnapshot_;
        GpuSceneLayout nextLayout = plan.targetLayout_;
        std::vector<GpuMeshBinding> nextMeshRegistry = plan.meshIndexRegistry_;
        std::vector<GpuInstanceBinding> nextInstanceRegistry = plan.instanceIndexRegistry_;

        snapshot_ = std::move(nextSnapshot);
        layout_ = std::move(nextLayout);
        meshIndexRegistry_ = std::move(nextMeshRegistry);
        instanceIndexRegistry_ = std::move(nextInstanceRegistry);
        nextMeshIndex_ = plan.nextMeshIndex_;
        nextInstanceIndex_ = plan.nextInstanceIndex_;
        // generation 表示 GPU 可见内容版本。稳定帧仍可发布最新 immutable snapshot，但不能迫使
        // 每个 frame slot 重建一份内容完全相同的 geometry/BLAS/TLAS。
        if (advancesGpuGeneration) {
            ++generation_;
        }
    }

    std::uint64_t GpuSceneUpdatePlanner::generation() const noexcept {
        return generation_;
    }

    const world::RenderWorldSnapshotPtr& GpuSceneUpdatePlanner::snapshot() const noexcept {
        return snapshot_;
    }

    const GpuSceneLayout& GpuSceneUpdatePlanner::layout() const noexcept {
        return layout_;
    }

    void GpuSceneUpdatePlanner::clear() noexcept {
        ++epoch_;
        if (epoch_ == 0) {
            epoch_ = 1;
        }
        generation_ = 0;
        snapshot_.reset();
        layout_ = {};
        meshIndexRegistry_.clear();
        instanceIndexRegistry_.clear();
        nextMeshIndex_ = 0;
        nextInstanceIndex_ = 0;
    }

} // namespace lumin::render::gpu

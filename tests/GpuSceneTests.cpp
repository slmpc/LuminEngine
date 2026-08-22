#include "render/gpu/GpuScene.hpp"
#include "render/gpu/GpuSceneResources.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

    using lumin::render::core::FrameSlotIndex;
    using lumin::render::gpu::BlasUpdateMode;
    using lumin::render::gpu::GpuInstancePatchMask;
    using lumin::render::gpu::GpuMaterialData;
    using lumin::render::gpu::GpuSceneCommitInfo;
    using lumin::render::gpu::GpuSceneUpdatePlan;
    using lumin::render::gpu::GpuSceneUpdatePlanner;
    using lumin::render::gpu::RenderInstanceId;
    using lumin::render::gpu::TlasUpdateMode;
    using lumin::render::world::RenderWorldCache;
    using lumin::render::world::SceneChangeMask;

    static_assert(!std::same_as<lumin::render::gpu::GpuMeshIndex, lumin::render::gpu::GpuInstanceIndex>);
    static_assert(!std::same_as<lumin::render::gpu::GpuInstanceIndex, lumin::render::gpu::GpuMaterialIndex>);
    static_assert(!std::is_copy_constructible_v<GpuSceneUpdatePlanner>);
    static_assert(std::is_same_v<std::underlying_type_t<lumin::scene::SurfaceModel>, std::uint32_t>);
    static_assert(static_cast<std::uint32_t>(lumin::scene::SurfaceModel::MetallicRoughness) == 0);
    static_assert(static_cast<std::uint32_t>(lumin::scene::SurfaceModel::BlinnPhong) == 1);
    static_assert(sizeof(GpuMaterialData) == 64 && alignof(GpuMaterialData) == 16);
    static_assert(sizeof(lumin::render::gpu::GpuPackedVertex) == 32);
    static_assert(sizeof(lumin::render::gpu::GpuInstanceData) == 144);
    static_assert(lumin::render::gpu::materialIndexFor(RenderInstanceId{lumin::scene::ModelHandle{7, 3}}).value() == 7);
    static_assert(offsetof(GpuMaterialData, baseColorMetallic) == 0);
    static_assert(offsetof(GpuMaterialData, specularColorShininess) == 16);
    static_assert(offsetof(GpuMaterialData, surfaceParameters) == 32);
    static_assert(offsetof(GpuMaterialData, metadata) == 48);
    static_assert(requires(const GpuSceneUpdatePlanner& planner, const lumin::render::world::SceneDelta& delta) {
        { planner.plan(delta) } -> std::same_as<GpuSceneUpdatePlan>;
        { planner.snapshot() } -> std::same_as<const lumin::render::world::RenderWorldSnapshotPtr&>;
    });

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Callable> void requireThrows(Callable&& callable, const char* message) {
        try {
            std::forward<Callable>(callable)();
        } catch (const Exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    [[nodiscard]] lumin::assets::Mesh makeTriangle(std::string name, float xOffset = 0.0f) {
        lumin::assets::Mesh mesh;
        mesh.name = std::move(name);
        mesh.vertices = {
            {{xOffset + 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{xOffset + 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{xOffset + 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        mesh.indices = {0, 1, 2};
        return mesh;
    }

    [[nodiscard]] GpuSceneCommitInfo successfulCommit(std::uint32_t frameSlot = 0) {
        return GpuSceneCommitInfo{FrameSlotIndex{frameSlot}, true, true, true};
    }

    struct Fixture {
        lumin::scene::Level level;
        lumin::scene::MeshHandle mesh = level.addMesh(makeTriangle("scene"));
        lumin::scene::ModelHandle model = level.addModel(mesh);
        RenderWorldCache world;
        GpuSceneUpdatePlanner planner;

        void initialize() {
            const auto delta = world.sync(level);
            const GpuSceneUpdatePlan plan = planner.plan(delta);
            planner.commit(plan, successfulCommit());
        }
    };

    class FakeBuffer final : public nvrhi::RefCounter<nvrhi::IBuffer> {
    public:
        explicit FakeBuffer(nvrhi::BufferDesc desc) : desc_(std::move(desc)) {
        }

        [[nodiscard]] const nvrhi::BufferDesc& getDesc() const override {
            return desc_;
        }

        [[nodiscard]] nvrhi::GpuVirtualAddress getGpuVirtualAddress() const override {
            return 0;
        }

    private:
        nvrhi::BufferDesc desc_;
    };

    class FakeAccelerationStructure final : public nvrhi::RefCounter<nvrhi::rt::IAccelStruct> {
    public:
        explicit FakeAccelerationStructure(nvrhi::rt::AccelStructDesc desc) : desc_(std::move(desc)) {
        }

        [[nodiscard]] const nvrhi::rt::AccelStructDesc& getDesc() const override {
            return desc_;
        }

        [[nodiscard]] bool isCompacted() const override {
            return false;
        }

        [[nodiscard]] std::uint64_t getDeviceAddress() const override {
            return 0x1000U;
        }

    private:
        nvrhi::rt::AccelStructDesc desc_;
    };

    class FakeGpuSceneBackend final : public lumin::render::gpu::GpuSceneBackend {
    public:
        std::vector<nvrhi::BufferDesc> bufferDescs;
        std::vector<nvrhi::rt::AccelStructDesc> accelerationStructureDescs;
        std::unordered_map<std::string, std::vector<std::byte>> writes;
        std::size_t blasBuilds = 0;
        std::size_t tlasBuilds = 0;
        std::vector<std::vector<nvrhi::rt::InstanceDesc>> tlasBuildInstances;
        std::vector<std::string> executionEvents;

        [[nodiscard]] nvrhi::BufferHandle createBuffer(const nvrhi::BufferDesc& desc) override {
            bufferDescs.push_back(desc);
            nvrhi::BufferHandle result = nvrhi::BufferHandle::Create(new FakeBuffer(desc));
            bufferNames_[result.Get()] = desc.debugName;
            return result;
        }

        [[nodiscard]] nvrhi::rt::AccelStructHandle
        createAccelerationStructure(const nvrhi::rt::AccelStructDesc& desc) override {
            accelerationStructureDescs.push_back(desc);
            return nvrhi::rt::AccelStructHandle::Create(new FakeAccelerationStructure(desc));
        }

        void writeBuffer(nvrhi::ICommandList*, nvrhi::IBuffer* buffer, const void* data, std::size_t size) override {
            require(buffer != nullptr && data != nullptr && size != 0,
                    "FrameGraph upload pass must provide valid buffer contents.");
            auto& destination = writes[bufferNames_.at(buffer)];
            destination.resize(size);
            std::memcpy(destination.data(), data, size);
        }

        void buildBottomLevel(nvrhi::ICommandList*, nvrhi::rt::IAccelStruct* blas,
                              const nvrhi::rt::GeometryDesc& geometry,
                              nvrhi::rt::AccelStructBuildFlags flags) override {
            require(blas != nullptr && geometry.geometryData.triangles.vertexStride == 32,
                    "BLAS build must consume the shared packed-vertex ABI.");
            require((flags & nvrhi::rt::AccelStructBuildFlags::AllowUpdate) != nvrhi::rt::AccelStructBuildFlags::None,
                    "Copy-on-write BLAS builds must retain future update compatibility.");
            executionEvents.emplace_back("blas-build");
            ++blasBuilds;
        }

        void buildTopLevel(nvrhi::ICommandList*, nvrhi::rt::IAccelStruct* tlas,
                           std::span<const nvrhi::rt::InstanceDesc> instances,
                           nvrhi::rt::AccelStructBuildFlags) override {
            require(tlas != nullptr && !instances.empty(), "TLAS build must reference active BLAS instances.");
            tlasBuildInstances.emplace_back(instances.begin(), instances.end());
            executionEvents.emplace_back("tlas-build");
            ++tlasBuilds;
        }

        [[nodiscard]] const nvrhi::BufferDesc& bufferDesc(const std::string& name) const {
            const auto found = std::ranges::find_if(bufferDescs, [&](const nvrhi::BufferDesc& desc) {
                return desc.debugName == name;
            });
            if (found == bufferDescs.end()) {
                throw std::runtime_error("Missing fake GPU buffer descriptor: " + name);
            }
            return *found;
        }

        template <typename T> [[nodiscard]] std::span<const T> records(const std::string& name) const {
            const auto& bytes = writes.at(name);
            require(bytes.size() % sizeof(T) == 0, "Uploaded GPU record bytes must preserve ABI stride.");
            return {reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)};
        }

    private:
        std::unordered_map<nvrhi::IBuffer*, std::string> bufferNames_;
    };

    class RecordingGpuSceneBarriers final : public lumin::render::FrameGraphBarrierRecorder {
    public:
        explicit RecordingGpuSceneBarriers(std::vector<std::string>* events = nullptr) : events_(events) {
        }

        std::size_t copyDestBuffers = 0;
        std::size_t accelerationStructureInputs = 0;
        std::size_t shaderResourceBuffers = 0;
        std::size_t accelerationStructureWrites = 0;
        std::size_t accelerationStructureReads = 0;

        void beginTrackingTextureState(nvrhi::ITexture*, nvrhi::TextureSubresourceSet, nvrhi::ResourceStates) override {
        }
        void beginTrackingBufferState(nvrhi::IBuffer*, nvrhi::ResourceStates) override {
        }
        void setTextureState(nvrhi::ITexture*, nvrhi::TextureSubresourceSet, nvrhi::ResourceStates) override {
        }
        void setBufferState(nvrhi::IBuffer*, nvrhi::ResourceStates state) override {
            copyDestBuffers += state == nvrhi::ResourceStates::CopyDest ? 1U : 0U;
            accelerationStructureInputs += state == nvrhi::ResourceStates::AccelStructBuildInput ? 1U : 0U;
            shaderResourceBuffers += state == nvrhi::ResourceStates::ShaderResource ? 1U : 0U;
        }
        void setAccelerationStructureState(nvrhi::rt::IAccelStruct*, nvrhi::ResourceStates state) override {
            accelerationStructureWrites += state == nvrhi::ResourceStates::AccelStructWrite ? 1U : 0U;
            accelerationStructureReads += state == nvrhi::ResourceStates::AccelStructRead ? 1U : 0U;
            if (events_ != nullptr && state == nvrhi::ResourceStates::AccelStructRead) {
                events_->emplace_back("as-read-barrier");
            }
        }
        void commitBarriers() override {
            if (events_ != nullptr) {
                events_->emplace_back("commit-barriers");
            }
        }

    private:
        std::vector<std::string>* events_ = nullptr;
    };

    void testSurfaceModelGpuContract() {
        const lumin::scene::Material defaults;
        require(defaults.surfaceModel == lumin::scene::SurfaceModel::MetallicRoughness,
                "Materials must default to the Metallic-Roughness surface model.");
        const GpuMaterialData defaultGpu = lumin::render::gpu::packGpuMaterial(defaults);
        require(defaultGpu.metadata.x == 0 && defaultGpu.metadata.y == 0 && defaultGpu.metadata.z == 0 &&
                    std::abs(defaultGpu.surfaceParameters.x - 0.45f) < 1e-6f,
                "Default GPU material packing must preserve PBR defaults and fallback descriptor zero.");

        lumin::scene::Material blinnPhong;
        blinnPhong.surfaceModel = lumin::scene::SurfaceModel::BlinnPhong;
        blinnPhong.albedo = {0.3f, 0.5f, 0.7f};
        blinnPhong.blinnPhong.specularColor = {0.2f, 0.4f, 0.8f};
        blinnPhong.blinnPhong.shininess = 32.0f;
        blinnPhong.textureScale = -2.0f;
        blinnPhong.textures = lumin::scene::PbrTextureSet{"base.png", "normal.png", "roughness.png", true};
        const GpuMaterialData gpu = lumin::render::gpu::packGpuMaterial(blinnPhong, 17);
        const float expectedRoughness = std::sqrt(2.0f / 34.0f);
        require(gpu.metadata == glm::uvec4(1U, 17U, 1U, 0U) &&
                    gpu.baseColorMetallic == glm::vec4(0.3f, 0.5f, 0.7f, 0.0f) &&
                    gpu.specularColorShininess == glm::vec4(0.2f, 0.4f, 0.8f, 32.0f) &&
                    std::abs(gpu.surfaceParameters.x - expectedRoughness) < 1e-6f && gpu.surfaceParameters.y == 2.0f &&
                    gpu.surfaceParameters.z == -1.0f && gpu.surfaceParameters.w == 0.0f,
                "Blinn-Phong GPU packing must preserve direct-lighting data and derive canonical NRD roughness.");

        lumin::scene::Material incompleteTextures;
        incompleteTextures.textures = lumin::scene::PbrTextureSet{.baseColor = "base-color.png"};
        const GpuMaterialData incompleteGpu = lumin::render::gpu::packGpuMaterial(incompleteTextures, 17U);
        require(incompleteGpu.metadata.y == 0U && incompleteGpu.metadata.z == 0U &&
                    incompleteGpu.surfaceParameters.z == 1.0f,
                "Incomplete texture sets must not expose a descriptor or normal-map convention to shaders.");
    }

    void testSurfaceModelChangesPatchOnlyMaterialData() {
        Fixture fixture;
        fixture.initialize();

        lumin::scene::Material material = fixture.level.model(fixture.model).material;
        material.surfaceModel = lumin::scene::SurfaceModel::BlinnPhong;
        material.blinnPhong.specularColor = {0.12f, 0.24f, 0.48f};
        material.blinnPhong.shininess = 96.0f;
        require(fixture.level.setModelMaterial(fixture.model, material), "Surface-model update must succeed.");

        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);
        const lumin::scene::Material& snapshotMaterial = delta.snapshot->findInstance(fixture.model)->model.material;
        require(snapshotMaterial.surfaceModel == lumin::scene::SurfaceModel::BlinnPhong &&
                    snapshotMaterial.blinnPhong == material.blinnPhong,
                "Immutable render snapshots must retain the complete Blinn-Phong material.");
        require(delta.changes == SceneChangeMask::TransformOrMaterial && plan.instancePatches().size() == 1 &&
                    lumin::render::gpu::hasAnyPatch(plan.instancePatches()[0].fields, GpuInstancePatchMask::Material) &&
                    !plan.rebuildsMaterialBindings() && plan.geometryUploads().empty() &&
                    !plan.rebuildsInstanceTopology() && plan.blasDecisions()[0].mode == BlasUpdateMode::Reuse &&
                    plan.tlasDecision() == TlasUpdateMode::Reuse,
                "Surface-model parameters must patch only material data without rebuilding bindings or AS state.");
    }

    void testFirstFrameBuildsEveryGpuDomain() {
        Fixture fixture;
        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);

        require(plan.initializesScene() && plan.changes() == SceneChangeMask::All,
                "The first GPU scene plan must initialize every domain.");
        require(!plan.previousSnapshot() && plan.targetSnapshot() == delta.snapshot,
                "The first plan must pin its target immutable snapshot.");
        require(plan.geometryUploads().size() == 1 && plan.geometryUploads()[0].requiresAllocationResize,
                "The first plan must allocate and upload every referenced mesh.");
        require(plan.rebuildsInstanceTopology() && plan.instanceRecords().size() == 1 && plan.instancePatches().empty(),
                "The first plan must write a complete instance table.");
        require(plan.rebuildsMaterialBindings() && plan.lightPatches().size() == 1,
                "The first plan must initialize material bindings and the sun record.");
        require(plan.blasDecisions().size() == 1 && plan.blasDecisions()[0].mode == BlasUpdateMode::Build &&
                    plan.tlasDecision() == TlasUpdateMode::Build,
                "The first plan must build BLAS and TLAS.");

        const auto* instance = plan.targetLayout().findInstance(RenderInstanceId{fixture.model});
        require(instance != nullptr && instance->instanceIndex.value() == 0 && instance->meshIndex.value() == 0 &&
                    instance->materialIndex.value() == 0 &&
                    plan.targetLayout().sunLight() == lumin::render::gpu::sunLightGpuIndex,
                "The first scene must receive deterministic typed GPU indices.");

        requireThrows<std::logic_error>(
            [&] {
                fixture.planner.commit(plan, GpuSceneCommitInfo{FrameSlotIndex{0}, false, true, true});
            },
            "GPU scene writes before the frame-slot fence wait must be rejected.");
        require(!fixture.planner.snapshot() && fixture.planner.generation() == 0,
                "A rejected commit must not publish CPU-side GPU scene state.");

        fixture.planner.commit(plan, successfulCommit());
        require(fixture.planner.snapshot() == delta.snapshot && fixture.planner.generation() == 1,
                "A successfully submitted first plan must become the committed scene.");
    }

    void testTransformOnlyPatchesOneInstanceAndUpdatesTlas() {
        Fixture fixture;
        fixture.initialize();
        const auto originalBinding = *fixture.planner.layout().findInstance(RenderInstanceId{fixture.model});

        lumin::scene::Transform transform;
        transform.position = {3.0f, 4.0f, 5.0f};
        require(fixture.level.setModelTransform(fixture.model, transform), "Transform update must succeed.");
        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);

        require(delta.changes == SceneChangeMask::TransformOrMaterial && plan.geometryUploads().empty() &&
                    !plan.rebuildsInstanceTopology() && plan.instanceRecords().empty(),
                "A transform-only edit must preserve geometry and instance topology.");
        require(
            plan.instancePatches().size() == 1 &&
                lumin::render::gpu::hasAnyPatch(plan.instancePatches()[0].fields, GpuInstancePatchMask::Transform) &&
                !lumin::render::gpu::hasAnyPatch(plan.instancePatches()[0].fields, GpuInstancePatchMask::Material),
            "A transform-only edit must emit exactly one transform patch.");
        require(!plan.rebuildsMaterialBindings() && plan.lightPatches().empty() &&
                    plan.blasDecisions()[0].mode == BlasUpdateMode::Reuse &&
                    plan.tlasDecision() == TlasUpdateMode::Update,
                "Transform-only updates must reuse BLAS and update TLAS.");

        const auto* targetBinding = plan.targetLayout().findInstance(RenderInstanceId{fixture.model});
        require(targetBinding != nullptr && targetBinding->instanceIndex == originalBinding.instanceIndex &&
                    targetBinding->meshIndex == originalBinding.meshIndex &&
                    targetBinding->materialIndex == originalBinding.materialIndex,
                "Transform changes must preserve every stable GPU index.");
    }

    void testGeometryChangeUploadsAndRefitsBlas() {
        Fixture fixture;
        fixture.initialize();
        const auto originalMesh = fixture.planner.layout().meshes().front().gpuIndex;

        require(fixture.level.replaceMesh(fixture.mesh, makeTriangle("moved", 2.0f)), "Mesh replacement must succeed.");
        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);

        require(delta.changes == SceneChangeMask::Geometry && plan.geometryUploads().size() == 1 &&
                    !plan.geometryUploads()[0].requiresAllocationResize &&
                    plan.geometryUploads()[0].replacesLiveAllocation && plan.requiresInFlightResourcePreservation(),
                "A same-topology geometry edit must upload without resizing its allocation.");
        require(plan.targetLayout().meshes().front().gpuIndex == originalMesh &&
                    plan.blasDecisions()[0].mode == BlasUpdateMode::Refit &&
                    plan.tlasDecision() == TlasUpdateMode::Update,
                "Same-topology vertex motion must preserve the mesh index, refit BLAS, and update TLAS.");
        require(!plan.rebuildsInstanceTopology() && plan.instancePatches().empty(),
                "Geometry edits must not rewrite unchanged instance records.");
        requireThrows<std::logic_error>(
            [&] {
                fixture.planner.commit(plan, GpuSceneCommitInfo{FrameSlotIndex{0}, true, true, false});
            },
            "Replacing shared geometry must preserve physical versions used by in-flight frames.");
    }

    void testTopologyRebuildPreservesExistingIndices() {
        Fixture fixture;
        fixture.initialize();
        const auto originalInstance = *fixture.planner.layout().findInstance(RenderInstanceId{fixture.model});
        const auto originalMesh = fixture.planner.layout().meshes().front().gpuIndex;

        const auto added = fixture.level.addModel(fixture.mesh, {.position = {2.0f, 0.0f, 0.0f}});
        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);

        require(delta.changes == SceneChangeMask::InstanceTopology && plan.geometryUploads().empty() &&
                    plan.rebuildsInstanceTopology() && plan.instanceRecords().size() == 2,
                "Adding an instance of existing geometry must only rebuild instance topology.");
        const auto* existing = plan.targetLayout().findInstance(RenderInstanceId{fixture.model});
        const auto* appended = plan.targetLayout().findInstance(RenderInstanceId{added});
        require(existing != nullptr && appended != nullptr &&
                    existing->instanceIndex == originalInstance.instanceIndex &&
                    existing->materialIndex == originalInstance.materialIndex && existing->meshIndex == originalMesh &&
                    appended->instanceIndex != existing->instanceIndex &&
                    appended->materialIndex != existing->materialIndex,
                "Topology growth must preserve existing indices and allocate distinct new slots.");
        require(plan.rebuildsMaterialBindings() && plan.blasDecisions()[0].mode == BlasUpdateMode::Reuse &&
                    plan.tlasDecision() == TlasUpdateMode::Build,
                "Instance topology growth must rebuild material mappings and TLAS while reusing BLAS.");
        fixture.planner.commit(plan, successfulCommit());

        require(fixture.level.removeModel(fixture.model) && fixture.level.removeModel(added),
                "Removing both instances must succeed.");
        const GpuSceneUpdatePlan emptyPlan = fixture.planner.plan(fixture.world.sync(fixture.level));
        require(emptyPlan.retiredMeshes().size() == 1 && emptyPlan.retiredMeshes()[0] == originalMesh,
                "An unreferenced mesh slot must be retired from the active layout.");
        fixture.planner.commit(emptyPlan, successfulCommit());

        const auto restored = fixture.level.addModel(fixture.mesh);
        const GpuSceneUpdatePlan restoredPlan = fixture.planner.plan(fixture.world.sync(fixture.level));
        require(restoredPlan.targetLayout().meshes().front().gpuIndex == originalMesh &&
                    restoredPlan.targetLayout().findInstance(RenderInstanceId{restored}) != nullptr,
                "A stable mesh handle must recover its original GPU index after becoming active again.");
    }

    void testGenerationReuseKeepsSlotBasedMaterialIndexLive() {
        Fixture fixture;
        fixture.initialize();
        const lumin::scene::ModelHandle removed = fixture.model;
        require(fixture.level.removeModel(removed), "Generation-reuse fixture must remove its original instance.");
        const lumin::scene::ModelHandle replacement = fixture.level.addModel(fixture.mesh);
        require(replacement.index == removed.index && replacement.generation != removed.generation,
                "Level must reuse the released model slot with a new generation.");

        const GpuSceneUpdatePlan plan = fixture.planner.plan(fixture.world.sync(fixture.level));
        const auto* binding = plan.targetLayout().findInstance(RenderInstanceId{replacement});
        require(binding != nullptr && binding->materialIndex.value() == replacement.index &&
                    plan.retiredMaterials().empty(),
                "A new generation must reuse its slot-based material index without retiring the live slot.");
    }

    void testMaterialBindingChangeRebuildsOnlyMaterialMapping() {
        Fixture fixture;
        fixture.initialize();

        lumin::scene::Material material = fixture.level.model(fixture.model).material;
        material.albedo = {0.2f, 0.4f, 0.8f};
        material.textures =
            lumin::scene::PbrTextureSet{std::filesystem::path{"albedo.png"}, std::filesystem::path{"normal.png"},
                                        std::filesystem::path{"roughness.png"}, true};
        require(fixture.level.setModelMaterial(fixture.model, material), "Material update must succeed.");
        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);

        require(delta.has(SceneChangeMask::TransformOrMaterial) && delta.has(SceneChangeMask::MaterialBinding),
                "Texture selection changes must carry material and binding delta bits.");
        require(plan.instancePatches().size() == 1 &&
                    lumin::render::gpu::hasAnyPatch(plan.instancePatches()[0].fields, GpuInstancePatchMask::Material) &&
                    plan.rebuildsMaterialBindings(),
                "A texture-backed material edit must patch material data and rebuild descriptor mapping.");
        require(plan.geometryUploads().empty() && !plan.rebuildsInstanceTopology() &&
                    plan.blasDecisions()[0].mode == BlasUpdateMode::Reuse &&
                    plan.tlasDecision() == TlasUpdateMode::Reuse,
                "Material binding changes must not rebuild geometry acceleration structures.");
    }

    void testLightingChangePatchesStableSunIndex() {
        Fixture fixture;
        fixture.initialize();

        lumin::scene::DirectionalLight sun = fixture.level.environment().sun;
        sun.color = {0.5f, 0.7f, 1.0f};
        fixture.level.setSun(sun);
        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);

        require(delta.changes == SceneChangeMask::Lighting && plan.lightPatches().size() == 1 &&
                    plan.lightPatches()[0].lightIndex == lumin::render::gpu::sunLightGpuIndex,
                "A sun edit must patch the stable directional-light slot.");
        require(plan.geometryUploads().empty() && plan.instancePatches().empty() && !plan.rebuildsInstanceTopology() &&
                    !plan.rebuildsMaterialBindings() && plan.blasDecisions()[0].mode == BlasUpdateMode::Reuse &&
                    plan.tlasDecision() == TlasUpdateMode::Reuse,
                "Lighting-only changes must reuse all geometry and acceleration structures.");
    }

    void testNoChangeReusesEverythingWithoutFenceRequirement() {
        Fixture fixture;
        fixture.initialize();
        const auto committed = fixture.planner.snapshot();
        const std::uint64_t committedGeneration = fixture.planner.generation();

        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(delta);
        require(delta.changes == SceneChangeMask::None && !plan.hasGpuWork() && plan.geometryUploads().empty() &&
                    plan.instancePatches().empty() && plan.lightPatches().empty() && plan.blasDecisions().size() == 1 &&
                    plan.blasDecisions()[0].mode == BlasUpdateMode::Reuse &&
                    plan.tlasDecision() == TlasUpdateMode::Reuse,
                "An unchanged snapshot must produce a complete reuse decision with no GPU writes.");

        fixture.planner.commit(plan, {});
        require(fixture.planner.snapshot() == committed && fixture.planner.generation() == committedGeneration,
                "A no-op plan may publish without pretending that GPU commands were submitted or versioned.");
    }

    void testStableFrameSlotsStopAllocatingPhysicalVersions() {
        Fixture fixture;
        FakeGpuSceneBackend backend;
        lumin::render::gpu::GpuSceneResources resources(backend, {.frameSlotCount = 2, .rayTracingEnabled = true});

        const auto submitFrame = [&](std::uint32_t frameSlot) {
            const GpuSceneUpdatePlan plan = fixture.planner.plan(fixture.world.sync(fixture.level));
            lumin::render::FrameGraph graph;
            const auto update = resources.recordUpdate(graph, plan, FrameSlotIndex{frameSlot}, true);
            graph.execute({});
            fixture.planner.commit(plan, successfulCommit(frameSlot));
            resources.finishUpdate(update, true);
            return std::pair{update.uploadPass().isValid(), update.accelerationStructurePass().isValid()};
        };

        require(submitFrame(0) == std::pair{true, true},
                "The first frame slot must initialize geometry and acceleration structures.");
        require(submitFrame(1) == std::pair{true, true},
                "The second frame slot must catch up once to the committed GPU scene generation.");
        const std::size_t warmBufferCount = backend.bufferDescs.size();
        const std::size_t warmAsCount = backend.accelerationStructureDescs.size();

        for (std::uint32_t frame = 0; frame < 1000; ++frame) {
            require(submitFrame(frame % 2U) == std::pair{false, false},
                    "A synchronized stable frame slot must reuse its physical version without update passes.");
        }
        require(backend.bufferDescs.size() == warmBufferCount &&
                    backend.accelerationStructureDescs.size() == warmAsCount,
                "A thousand stable frames must not allocate additional geometry, tables, BLAS, or TLAS objects.");

        lumin::scene::Transform moved;
        moved.position = {3.0F, 0.0F, 0.0F};
        require(fixture.level.setModelTransform(fixture.model, moved), "Transform update must succeed.");
        require(submitFrame(0) == std::pair{true, true},
                "The first slot must materialize the changed GPU scene generation.");
        require(submitFrame(1) == std::pair{true, true},
                "The other slot must catch up exactly once even when its planner delta is otherwise empty.");
        const std::size_t updatedBufferCount = backend.bufferDescs.size();
        const std::size_t updatedAsCount = backend.accelerationStructureDescs.size();

        for (std::uint32_t frame = 0; frame < 1000; ++frame) {
            static_cast<void>(submitFrame(frame % 2U));
        }
        require(backend.bufferDescs.size() == updatedBufferCount &&
                    backend.accelerationStructureDescs.size() == updatedAsCount,
                "Slots synchronized after a scene edit must return to zero-allocation stable reuse.");
    }

    void testPlansPinOldSnapshotsAndRejectStaleCommit() {
        Fixture fixture;
        fixture.initialize();
        const auto originalSnapshot = fixture.planner.snapshot();

        lumin::scene::Transform firstTransform;
        firstTransform.position = {1.0f, 0.0f, 0.0f};
        require(fixture.level.setModelTransform(fixture.model, firstTransform), "First transform update must succeed.");
        const auto firstDelta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan firstPlan = fixture.planner.plan(firstDelta);

        lumin::scene::Transform secondTransform;
        secondTransform.position = {2.0f, 0.0f, 0.0f};
        require(fixture.level.setModelTransform(fixture.model, secondTransform),
                "Second transform update must succeed.");
        const auto secondDelta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan competingPlan = fixture.planner.plan(secondDelta);

        require(firstPlan.previousSnapshot() == originalSnapshot && firstPlan.targetSnapshot() == firstDelta.snapshot &&
                    firstPlan.targetSnapshot()->findInstance(fixture.model)->model.transform.position ==
                        firstTransform.position &&
                    secondDelta.snapshot->findInstance(fixture.model)->model.transform.position ==
                        secondTransform.position,
                "Outstanding plans must pin immutable snapshots despite later scene synchronization.");
        require(fixture.planner.snapshot() == originalSnapshot,
                "Planning must not advance the committed snapshot before submit.");

        fixture.planner.commit(firstPlan, successfulCommit());
        requireThrows<std::logic_error>(
            [&] {
                fixture.planner.commit(competingPlan, successfulCommit());
            },
            "A plan based on an older committed generation must be rejected.");
        require(fixture.planner.snapshot() == firstDelta.snapshot,
                "Rejecting a stale plan must preserve the successfully committed snapshot.");

        const lumin::render::world::SceneDelta replayDelta{SceneChangeMask::TransformOrMaterial, secondDelta.snapshot};
        const GpuSceneUpdatePlan replayPlan = fixture.planner.plan(replayDelta);
        fixture.planner.commit(replayPlan, successfulCommit());
        require(fixture.planner.snapshot() == secondDelta.snapshot &&
                    firstDelta.snapshot->findInstance(fixture.model)->model.transform.position ==
                        firstTransform.position,
                "Replanning from the new base must publish the latest snapshot without mutating the older one.");
    }

    void testPlannerResetInvalidatesOutstandingPlans() {
        Fixture fixture;
        const auto delta = fixture.world.sync(fixture.level);
        const GpuSceneUpdatePlan abandonedPlan = fixture.planner.plan(delta);
        fixture.planner.clear();

        requireThrows<std::logic_error>(
            [&] {
                fixture.planner.commit(abandonedPlan, successfulCommit());
            },
            "Resetting the planner must invalidate every outstanding plan from the previous allocation epoch.");
        const GpuSceneUpdatePlan replacementPlan = fixture.planner.plan(delta);
        fixture.planner.commit(replacementPlan, successfulCommit());
        require(fixture.planner.generation() == 1 && fixture.planner.snapshot() == delta.snapshot,
                "A reset planner must accept a newly generated full initialization plan.");
    }

    void testPhysicalResourcesFallbackAndSubmissionTransaction() {
        Fixture fixture;
        const GpuSceneUpdatePlan plan = fixture.planner.plan(fixture.world.sync(fixture.level));
        FakeGpuSceneBackend backend;
        lumin::render::gpu::GpuSceneResources resources(backend, {.frameSlotCount = 2, .rayTracingEnabled = false});

        lumin::render::FrameGraph rejectedGraph;
        requireThrows<std::logic_error>(
            [&] {
                (void)resources.recordUpdate(rejectedGraph, plan, FrameSlotIndex{0}, false);
            },
            "Physical GPU scene writes before the slot fence wait must be rejected.");
        require(backend.bufferDescs.empty(), "Rejected pre-fence updates must not allocate physical resources.");

        lumin::render::FrameGraph discardedGraph;
        const auto discarded = resources.recordUpdate(discardedGraph, plan, FrameSlotIndex{0}, true);
        require(discarded.isValid() && discarded.uploadPass().isValid() &&
                    !discarded.accelerationStructurePass().isValid(),
                "RT fallback must record only the shared buffer upload pass.");
        require(backend.accelerationStructureDescs.empty(),
                "RT fallback must never call the acceleration-structure creation API.");
        require(!resources.descriptors(FrameSlotIndex{0}).meshes,
                "A candidate physical version must remain hidden before submit succeeds.");
        require(resources.candidateDescriptors(discarded).meshes &&
                    resources.candidateGeometry(discarded).size() == 1 && discarded.meshRecordsResource().isValid() &&
                    discarded.instanceRecordsResource().isValid() && discarded.materialRecordsResource().isValid() &&
                    discarded.lightRecordsResource().isValid() && discarded.geometryResources().size() == 1 &&
                    !discarded.tlasResource().isValid(),
                "Fallback passes must be able to bind pending candidate buffers without exposing a TLAS.");
        requireThrows<std::logic_error>(
            [&] {
                lumin::render::FrameGraph competingGraph;
                (void)resources.recordUpdate(competingGraph, plan, FrameSlotIndex{0}, true);
            },
            "A frame slot must not accept two unfinished physical updates.");
        discardedGraph.execute({});
        resources.finishUpdate(discarded, false);
        requireThrows<std::logic_error>(
            [&] {
                (void)resources.candidateDescriptors(discarded);
            },
            "A finished candidate ticket must not expose destroyed pending resources.");
        require(!resources.descriptors(FrameSlotIndex{0}).instances,
                "Discarding a failed submission must preserve the previously published empty slot.");
        require(backend.blasBuilds == 0 && backend.tlasBuilds == 0,
                "RT fallback upload execution must not build acceleration structures.");

        lumin::render::FrameGraph submittedGraph;
        const auto submitted = resources.recordUpdate(submittedGraph, plan, FrameSlotIndex{0}, true);
        submittedGraph.execute({});
        resources.finishUpdate(submitted, true);
        const auto published = resources.descriptors(FrameSlotIndex{0});
        require(published.meshes && published.instances && published.materials && published.lights && !published.tlas &&
                    !published.rayTracingEnabled && published.generation == 1,
                "Successful fallback submission must publish all shared buffers without a TLAS.");
        require(resources.geometry(FrameSlotIndex{0}).size() == 1 && !resources.geometry(FrameSlotIndex{0})[0].blas,
                "Fallback geometry descriptors must retain raster buffers and an empty BLAS.");

        const auto& vertexDesc = backend.bufferDesc("GpuScene.Vertices.0");
        const auto& instanceDesc = backend.bufferDesc("GpuScene.InstanceRecords");
        require(vertexDesc.structStride == sizeof(lumin::render::gpu::GpuPackedVertex) && vertexDesc.isVertexBuffer &&
                    !vertexDesc.isAccelStructBuildInput,
                "Fallback packed vertices must use the 32-byte raster/RT ABI without AS usage flags.");
        require(instanceDesc.structStride == sizeof(lumin::render::gpu::GpuInstanceData),
                "Instance structured buffers must expose the 144-byte shared shader ABI.");
        const auto instances = backend.records<lumin::render::gpu::GpuInstanceData>("GpuScene.InstanceRecords");
        require(instances[0].metadata == glm::uvec4(0U, 0U, 0U, 0U),
                "Single-geometry instance records must pack material, local ranges, and descriptor index zero.");
        const auto vertices = backend.records<lumin::render::gpu::GpuPackedVertex>("GpuScene.Vertices.0");
        require(vertices.size() == 3 && vertices[1].position.w == 1.0F && vertices[1].normal.w == 0.0F &&
                    vertices[2].position.w == 0.0F && vertices[2].normal.w == 1.0F,
                "Packed RT vertices must preserve mesh UVs in the two ABI padding lanes.");

        const nvrhi::IBuffer* oldInstances = published.instances;
        lumin::render::FrameGraph replacementGraph;
        const auto replacement = resources.recordUpdate(replacementGraph, plan, FrameSlotIndex{0}, true);
        require(resources.descriptors(FrameSlotIndex{0}).instances.Get() == oldInstances,
                "Copy-on-write preparation must retain the slot's published in-flight buffers.");
        replacementGraph.execute({});
        resources.finishUpdate(replacement, true);
        require(resources.descriptors(FrameSlotIndex{0}).instances.Get() != oldInstances,
                "A new physical version may replace the slot only after successful submission.");
    }

    void testPhysicalResourcesRecordBindlessGeometryAndAsPass() {
        Fixture fixture;
        const auto secondMesh = fixture.level.addMesh(makeTriangle("second", 5.0F));
        const auto secondModel = fixture.level.addModel(secondMesh, {.position = {2.0F, 0.0F, 0.0F}});
        const GpuSceneUpdatePlan plan = fixture.planner.plan(fixture.world.sync(fixture.level));
        FakeGpuSceneBackend backend;
        lumin::render::gpu::GpuSceneResources resources(backend, {.frameSlotCount = 2, .rayTracingEnabled = true});
        lumin::render::FrameGraph graph;

        const auto update = resources.recordUpdate(graph, plan, FrameSlotIndex{1}, true);
        require(update.uploadPass().isValid() && update.accelerationStructurePass().isValid(),
                "RT mode must produce separate upload and AS build passes.");
        const auto candidateDescriptors = resources.candidateDescriptors(update);
        const auto candidateGeometry = resources.candidateGeometry(update);
        require(candidateDescriptors.tlas && candidateGeometry.size() == 2 && update.tlasResource().isValid() &&
                    update.geometryResources().size() == candidateGeometry.size(),
                "Same-frame RT consumers must see the pending TLAS and ordered geometry descriptors.");
        for (std::size_t index = 0; index < candidateGeometry.size(); ++index) {
            require(update.geometryResources()[index].meshIndex == candidateGeometry[index].meshIndex &&
                        update.geometryResources()[index].vertices.isValid() &&
                        update.geometryResources()[index].indices.isValid(),
                    "Typed FrameGraph geometry handles must preserve candidate descriptor ordering.");
        }
        require(backend.accelerationStructureDescs.size() == 3 && backend.blasBuilds == 0 && backend.tlasBuilds == 0,
                "Recording must create two BLAS and one TLAS without executing build commands early.");
        for (const nvrhi::BufferDesc& desc : backend.bufferDescs) {
            if (desc.isVertexBuffer || desc.isIndexBuffer) {
                require(desc.isAccelStructBuildInput,
                        "RT geometry buffers must opt into NvRHI acceleration-structure build input usage.");
            }
        }

        const std::vector<lumin::render::gpu::GpuGeometryFrameGraphResources> traceGeometry(
            update.geometryResources().begin(), update.geometryResources().end());
        const std::array traceTables{update.meshRecordsResource(), update.instanceRecordsResource(),
                                     update.materialRecordsResource(), update.lightRecordsResource()};
        const auto traceTlas = update.tlasResource();
        bool traceObservedCompletedBuilds = false;
        graph.addPass(
            "gpu-scene-candidate-trace-probe", lumin::render::FrameGraphPassType::RayTracing,
            [traceGeometry, traceTables, traceTlas](lumin::render::FrameGraphBuilder& builder) {
                for (const auto& geometryResources : traceGeometry) {
                    builder.read(geometryResources.vertices, nvrhi::ResourceStates::ShaderResource);
                    builder.read(geometryResources.indices, nvrhi::ResourceStates::ShaderResource);
                }
                for (const auto table : traceTables) {
                    builder.read(table, nvrhi::ResourceStates::ShaderResource);
                }
                builder.readAccelerationStructure(traceTlas);
            },
            [&](const lumin::render::FrameGraphContext&) {
                traceObservedCompletedBuilds = backend.blasBuilds == 2 && backend.tlasBuilds == 1;
            });

        RecordingGpuSceneBarriers barriers(&backend.executionEvents);
        graph.execute({.barriers = &barriers});
        require(backend.blasBuilds == 2 && backend.tlasBuilds == 1 && traceObservedCompletedBuilds,
                "The candidate trace pass must execute only after every active BLAS and the TLAS are built.");
        require(!backend.tlasBuildInstances.empty() &&
                    std::ranges::all_of(backend.tlasBuildInstances.back(),
                                        [](const nvrhi::rt::InstanceDesc& instance) {
                                            return (instance.flags & nvrhi::rt::InstanceFlags::TriangleCullDisable) !=
                                                   nvrhi::rt::InstanceFlags::None;
                                        }),
                "RT instances must disable triangle culling to match the double-sided raster G-buffer contract.");
        require(barriers.copyDestBuffers == 8 && barriers.accelerationStructureInputs == 4 &&
                    barriers.shaderResourceBuffers == 8 && barriers.accelerationStructureWrites == 3 &&
                    barriers.accelerationStructureReads == 3,
                "FrameGraph must exclusively transition eight uploads, four geometry inputs, and three BLAS/TLAS.");
        const auto firstBlas = std::ranges::find(backend.executionEvents, std::string{"blas-build"});
        const auto tlas = std::ranges::find(backend.executionEvents, std::string{"tlas-build"});
        const auto lastBlas =
            firstBlas == backend.executionEvents.end()
                ? backend.executionEvents.end()
                : std::ranges::find(firstBlas + 1, backend.executionEvents.end(), std::string{"blas-build"});
        require(lastBlas != backend.executionEvents.end() && tlas != backend.executionEvents.end() && lastBlas < tlas &&
                    std::ranges::find(lastBlas + 1, tlas, std::string{"as-read-barrier"}) != tlas &&
                    std::ranges::find(lastBlas + 1, tlas, std::string{"commit-barriers"}) != tlas,
                "TLAS build must execute only after a committed BLAS write-to-read memory barrier.");
        const auto instances = backend.records<lumin::render::gpu::GpuInstanceData>("GpuScene.InstanceRecords");
        for (const auto model : {fixture.model, secondModel}) {
            const auto* binding = plan.targetLayout().findInstance(RenderInstanceId{model});
            require(binding != nullptr, "Physical resource test model must have a GPU instance binding.");
            const auto descriptor = std::ranges::find_if(candidateGeometry, [&](const auto& candidate) {
                return candidate.meshIndex == binding->meshIndex;
            });
            require(descriptor != candidateGeometry.end(),
                    "Every instance mesh must have a bindless geometry descriptor.");
            const auto descriptorIndex =
                static_cast<std::uint32_t>(std::distance(candidateGeometry.begin(), descriptor));
            require(instances[binding->instanceIndex.value()].metadata.w == descriptorIndex,
                    "Instance metadata.w must index the exact geometry(slot) descriptor used by closest-hit.");
        }

        const std::array candidateMeshOrder{candidateGeometry[0].meshIndex, candidateGeometry[1].meshIndex};
        resources.finishUpdate(update, true);
        const auto descriptors = resources.descriptors(FrameSlotIndex{1});
        const auto geometry = resources.geometry(FrameSlotIndex{1});
        require(descriptors.tlas && descriptors.rayTracingEnabled && geometry.size() == 2 && geometry[0].blas &&
                    geometry[1].blas && geometry[0].meshIndex == candidateMeshOrder[0] &&
                    geometry[1].meshIndex == candidateMeshOrder[1],
                "Successful RT submission must publish the same ordered candidate descriptors consumed this frame.");
    }

    void testSharedBlasInstancesRemainDistinctAcrossFrameSlots() {
        Fixture fixture;
        lumin::scene::Transform rightTransform;
        rightTransform.position = {2.0F, 0.0F, 0.0F};
        const auto rightModel = fixture.level.addModel(fixture.mesh, rightTransform);
        const GpuSceneUpdatePlan plan = fixture.planner.plan(fixture.world.sync(fixture.level));
        FakeGpuSceneBackend backend;
        lumin::render::gpu::GpuSceneResources resources(backend, {.frameSlotCount = 2, .rayTracingEnabled = true});

        for (std::uint32_t slot = 0; slot < 2; ++slot) {
            lumin::render::FrameGraph graph;
            const auto update = resources.recordUpdate(graph, plan, FrameSlotIndex{slot}, true);
            graph.execute({});
            resources.finishUpdate(update, true);
        }

        require(backend.blasBuilds == 2 && backend.tlasBuilds == 2 && backend.tlasBuildInstances.size() == 2,
                "Each frame slot must build one shared BLAS and a TLAS containing both instances.");
        const auto* leftBinding = plan.targetLayout().findInstance(RenderInstanceId{fixture.model});
        const auto* rightBinding = plan.targetLayout().findInstance(RenderInstanceId{rightModel});
        require(leftBinding != nullptr && rightBinding != nullptr &&
                    leftBinding->instanceIndex != rightBinding->instanceIndex,
                "Models sharing a mesh must keep distinct stable instance IDs.");

        for (const auto& instances : backend.tlasBuildInstances) {
            require(instances.size() == 2 && instances[0].bottomLevelAS != nullptr &&
                        instances[0].bottomLevelAS == instances[1].bottomLevelAS,
                    "Both TLAS instances must reference the same live BLAS.");
            require(instances[0].instanceID != instances[1].instanceID,
                    "Shared-BLAS TLAS entries must not overwrite each other's InstanceID.");
            const auto& right = instances[instances[0].instanceID == rightBinding->instanceIndex.value() ? 0U : 1U];
            require(right.instanceID == rightBinding->instanceIndex.value() &&
                        std::abs(right.transform[3] - 2.0F) < 1e-6F,
                    "The second shared-BLAS instance must preserve its independent world transform.");
        }
    }

} // namespace

int main() {
    try {
        testSurfaceModelGpuContract();
        testSurfaceModelChangesPatchOnlyMaterialData();
        testFirstFrameBuildsEveryGpuDomain();
        testTransformOnlyPatchesOneInstanceAndUpdatesTlas();
        testGeometryChangeUploadsAndRefitsBlas();
        testTopologyRebuildPreservesExistingIndices();
        testGenerationReuseKeepsSlotBasedMaterialIndexLive();
        testMaterialBindingChangeRebuildsOnlyMaterialMapping();
        testLightingChangePatchesStableSunIndex();
        testNoChangeReusesEverythingWithoutFenceRequirement();
        testStableFrameSlotsStopAllocatingPhysicalVersions();
        testPlansPinOldSnapshotsAndRejectStaleCommit();
        testPlannerResetInvalidatesOutstandingPlans();
        testPhysicalResourcesFallbackAndSubmissionTransaction();
        testPhysicalResourcesRecordBindlessGeometryAndAsPass();
        testSharedBlasInstancesRemainDistinctAcrossFrameSlots();
        std::puts("GpuScene PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GpuScene FAIL: %s\n", error.what());
        return 1;
    }
}

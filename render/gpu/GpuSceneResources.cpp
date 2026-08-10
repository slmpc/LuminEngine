#include "render/gpu/GpuSceneResources.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace lumin::render::gpu {
    namespace {

        struct BufferUpload {
            nvrhi::BufferHandle buffer;
            std::vector<std::byte> bytes;
        };

        template <typename T> [[nodiscard]] std::vector<std::byte> asBytes(std::span<const T> values) {
            std::vector<std::byte> result(values.size_bytes());
            if (!result.empty()) {
                std::memcpy(result.data(), values.data(), result.size());
            }
            return result;
        }

        template <typename Index> [[nodiscard]] std::size_t tableSize(std::span<const Index> indices) {
            std::size_t result = 1;
            for (const Index index : indices) {
                if (!index.isValid()) {
                    throw std::logic_error("GPU scene layout contains an invalid stable index.");
                }
                result = std::max(result, static_cast<std::size_t>(index.value()) + 1U);
            }
            return result;
        }

        [[nodiscard]] nvrhi::BufferDesc structuredBufferDesc(std::uint64_t byteSize, std::uint32_t stride,
                                                             std::string name) {
            nvrhi::BufferDesc desc;
            desc.byteSize = byteSize;
            desc.structStride = stride;
            desc.debugName = std::move(name);
            desc.initialState = nvrhi::ResourceStates::Common;
            desc.keepInitialState = false;
            return desc;
        }

        [[nodiscard]] glm::mat4 makeNormalMatrix(const glm::mat4& model) {
            glm::mat4 result = glm::inverseTranspose(model);
            result[0][3] = 0.0F;
            result[1][3] = 0.0F;
            result[2][3] = 0.0F;
            result[3] = glm::vec4{0.0F, 0.0F, 0.0F, 1.0F};
            return result;
        }

        void copyAffineTransform(nvrhi::rt::InstanceDesc& instance, const glm::mat4& matrix) {
            nvrhi::rt::AffineTransform transform{};
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    transform[row * 4U + column] = matrix[column][row];
                }
            }
            instance.setTransform(transform);
        }

        [[nodiscard]] GpuDirectionalLightData packLight(const scene::DirectionalLight& light) {
            glm::vec3 direction = light.direction;
            const float length = glm::length(direction);
            if (length > 0.0F) {
                direction /= length;
            }
            return GpuDirectionalLightData{
                .directionIlluminance = glm::vec4{direction, std::max(light.illuminanceLux, 0.0F)},
                .colorCastsShadows = glm::vec4{light.color, light.castsShadows ? 1.0F : 0.0F},
            };
        }

    } // namespace

    struct GpuSceneResources::SlotVersion {
        GpuSceneDescriptors descriptors;
        std::vector<GpuGeometryDescriptor> geometry;
        std::vector<BufferUpload> uploads;
        std::vector<nvrhi::rt::GeometryDesc> blasGeometry;
        std::vector<nvrhi::rt::InstanceDesc> tlasInstances;
    };

    struct GpuSceneResources::PendingVersion {
        std::uint64_t serial = 0;
        std::shared_ptr<const SlotVersion> version;
        // 仅新建候选需要在提交后释放 CPU upload/build 描述；复用已发布版本时为空。
        std::shared_ptr<SlotVersion> writableCandidate;
    };

    NvrhiGpuSceneBackend::NvrhiGpuSceneBackend(nvrhi::IDevice& device) noexcept : device_(device) {
    }

    nvrhi::BufferHandle NvrhiGpuSceneBackend::createBuffer(const nvrhi::BufferDesc& desc) {
        return device_.createBuffer(desc);
    }

    nvrhi::rt::AccelStructHandle
    NvrhiGpuSceneBackend::createAccelerationStructure(const nvrhi::rt::AccelStructDesc& desc) {
        return device_.createAccelStruct(desc);
    }

    void NvrhiGpuSceneBackend::writeBuffer(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, const void* data,
                                           std::size_t size) {
        if (commandList == nullptr) {
            throw std::invalid_argument("GPU scene uploads require an NvRHI command list.");
        }
        commandList->writeBuffer(buffer, data, size);
    }

    void NvrhiGpuSceneBackend::buildBottomLevel(nvrhi::ICommandList* commandList, nvrhi::rt::IAccelStruct* blas,
                                                const nvrhi::rt::GeometryDesc& geometry,
                                                nvrhi::rt::AccelStructBuildFlags flags) {
        if (commandList == nullptr) {
            throw std::invalid_argument("BLAS builds require an NvRHI command list.");
        }
        commandList->buildBottomLevelAccelStruct(blas, &geometry, 1, flags);
    }

    void NvrhiGpuSceneBackend::buildTopLevel(nvrhi::ICommandList* commandList, nvrhi::rt::IAccelStruct* tlas,
                                             std::span<const nvrhi::rt::InstanceDesc> instances,
                                             nvrhi::rt::AccelStructBuildFlags flags) {
        if (commandList == nullptr) {
            throw std::invalid_argument("TLAS builds require an NvRHI command list.");
        }
        commandList->buildTopLevelAccelStruct(tlas, instances.data(), instances.size(), flags);
    }

    bool GpuScenePreparedUpdate::isValid() const noexcept {
        return owner_ != nullptr && frameSlot_.isValid() && serial_ != 0;
    }

    core::FrameSlotIndex GpuScenePreparedUpdate::frameSlot() const noexcept {
        return frameSlot_;
    }

    std::uint64_t GpuScenePreparedUpdate::generation() const noexcept {
        return generation_;
    }

    FrameGraphPassHandle GpuScenePreparedUpdate::uploadPass() const noexcept {
        return uploadPass_;
    }

    FrameGraphPassHandle GpuScenePreparedUpdate::accelerationStructurePass() const noexcept {
        return accelerationStructurePass_;
    }

    std::span<const FrameGraphResourceHandle> GpuScenePreparedUpdate::bufferResources() const noexcept {
        return bufferResources_;
    }

    FrameGraphResourceHandle GpuScenePreparedUpdate::meshRecordsResource() const noexcept {
        return meshRecordsResource_;
    }

    FrameGraphResourceHandle GpuScenePreparedUpdate::instanceRecordsResource() const noexcept {
        return instanceRecordsResource_;
    }

    FrameGraphResourceHandle GpuScenePreparedUpdate::materialRecordsResource() const noexcept {
        return materialRecordsResource_;
    }

    FrameGraphResourceHandle GpuScenePreparedUpdate::lightRecordsResource() const noexcept {
        return lightRecordsResource_;
    }

    std::span<const GpuGeometryFrameGraphResources> GpuScenePreparedUpdate::geometryResources() const noexcept {
        return geometryResources_;
    }

    FrameGraphResourceHandle GpuScenePreparedUpdate::tlasResource() const noexcept {
        return topLevelAccelerationStructureResource_;
    }

    FrameGraphResourceHandle GpuScenePreparedUpdate::topLevelAccelerationStructureResource() const noexcept {
        return tlasResource();
    }

    GpuSceneResources::GpuSceneResources(GpuSceneBackend& backend, GpuSceneResourceConfig config)
        : backend_(backend), config_(config) {
        if (config_.frameSlotCount == 0) {
            throw std::invalid_argument("GPU scene resources require at least one frame slot.");
        }
        slots_.resize(config_.frameSlotCount);
        pending_.resize(config_.frameSlotCount);
    }

    GpuSceneResources::~GpuSceneResources() = default;

    std::size_t GpuSceneResources::validateSlot(core::FrameSlotIndex frameSlot) const {
        if (!frameSlot.isValid() || frameSlot.value() >= config_.frameSlotCount) {
            throw std::out_of_range("GPU scene frame slot is outside the configured range.");
        }
        return frameSlot.value();
    }

    GpuScenePreparedUpdate GpuSceneResources::recordUpdate(FrameGraph& frameGraph, const GpuSceneUpdatePlan& plan,
                                                           core::FrameSlotIndex frameSlot, bool frameSlotFenceWaited) {
        const std::size_t slot = validateSlot(frameSlot);
        if (!frameSlotFenceWaited) {
            throw std::logic_error("GPU scene resources may only be replaced after waiting for the slot fence.");
        }
        if (pending_[slot]) {
            throw std::logic_error("GPU scene frame slot already has an unfinished physical update.");
        }
        if (!plan.targetSnapshot()) {
            throw std::invalid_argument("GPU scene physical updates require a target snapshot.");
        }
        if (plan.hasGpuWork() && plan.baseGeneration() == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("GPU scene physical generation is exhausted.");
        }

        const std::uint64_t targetGeneration = plan.baseGeneration() + (plan.hasGpuWork() ? 1U : 0U);

        // 稳定帧只需把该 frame slot 已提交的资源重新导入本帧 FrameGraph。资源状态仍由 FrameGraph
        // 声明和转换，但不会创建 buffer/AS，也不会录制上传或 build 命令。
        if (!plan.hasGpuWork() && slots_[slot] && slots_[slot]->descriptors.generation == targetGeneration) {
            const std::shared_ptr<const SlotVersion> version = slots_[slot];
            GpuScenePreparedUpdate ticket;
            ticket.owner_ = this;
            ticket.frameSlot_ = frameSlot;
            ticket.serial_ = nextSerial_++;
            ticket.generation_ = targetGeneration;

            ticket.bufferResources_.reserve(version->geometry.size() * 2U + 4U);
            ticket.geometryResources_.reserve(version->geometry.size());
            for (std::size_t index = 0; index < version->geometry.size(); ++index) {
                const GpuGeometryDescriptor& geometry = version->geometry[index];
                const FrameGraphResourceHandle vertices = frameGraph.importBuffer(
                    "gpu-scene-reused-vertices-" + std::to_string(index),
                    FrameGraphBufferDesc{.size = geometry.vertices->getDesc().byteSize,
                                         .buffer = geometry.vertices,
                                         .initialState = nvrhi::ResourceStates::ShaderResource,
                                         .finalState = nvrhi::ResourceStates::ShaderResource});
                const FrameGraphResourceHandle indices = frameGraph.importBuffer(
                    "gpu-scene-reused-indices-" + std::to_string(index),
                    FrameGraphBufferDesc{.size = geometry.indices->getDesc().byteSize,
                                         .buffer = geometry.indices,
                                         .initialState = nvrhi::ResourceStates::ShaderResource,
                                         .finalState = nvrhi::ResourceStates::ShaderResource});
                ticket.bufferResources_.push_back(vertices);
                ticket.bufferResources_.push_back(indices);
                ticket.geometryResources_.push_back(
                    GpuGeometryFrameGraphResources{geometry.meshIndex, vertices, indices});
            }

            const auto importTable = [&](const char* name, const nvrhi::BufferHandle& buffer) {
                const FrameGraphResourceHandle resource = frameGraph.importBuffer(
                    name, FrameGraphBufferDesc{.size = buffer->getDesc().byteSize,
                                               .buffer = buffer,
                                               .initialState = nvrhi::ResourceStates::ShaderResource,
                                               .finalState = nvrhi::ResourceStates::ShaderResource});
                ticket.bufferResources_.push_back(resource);
                return resource;
            };
            ticket.meshRecordsResource_ = importTable("gpu-scene-reused-mesh-records", version->descriptors.meshes);
            ticket.instanceRecordsResource_ =
                importTable("gpu-scene-reused-instance-records", version->descriptors.instances);
            ticket.materialRecordsResource_ =
                importTable("gpu-scene-reused-material-records", version->descriptors.materials);
            ticket.lightRecordsResource_ =
                importTable("gpu-scene-reused-light-records", version->descriptors.lights);

            if (config_.rayTracingEnabled) {
                for (std::size_t index = 0; index < version->geometry.size(); ++index) {
                    if (version->geometry[index].blas) {
                        static_cast<void>(frameGraph.importAccelerationStructure(
                            "gpu-scene-reused-blas-" + std::to_string(index),
                            FrameGraphAccelerationStructureDesc{
                                .accelerationStructure = version->geometry[index].blas,
                                .initialState = nvrhi::ResourceStates::AccelStructRead,
                                .finalState = nvrhi::ResourceStates::AccelStructRead}));
                    }
                }
                ticket.topLevelAccelerationStructureResource_ = frameGraph.importAccelerationStructure(
                    "gpu-scene-reused-tlas",
                    FrameGraphAccelerationStructureDesc{.accelerationStructure = version->descriptors.tlas,
                                                        .initialState = nvrhi::ResourceStates::AccelStructRead,
                                                        .finalState = nvrhi::ResourceStates::AccelStructRead});
            }

            pending_[slot] = std::make_shared<PendingVersion>(PendingVersion{ticket.serial_, version, nullptr});
            return ticket;
        }

        auto version = std::make_shared<SlotVersion>();
        version->descriptors.generation = targetGeneration;
        version->descriptors.rayTracingEnabled = config_.rayTracingEnabled;

        const auto& snapshot = *plan.targetSnapshot();
        const auto& layout = plan.targetLayout();

        std::vector<GpuMeshIndex> meshIndices;
        meshIndices.reserve(layout.meshes().size());
        for (const GpuMeshBinding& binding : layout.meshes()) {
            meshIndices.push_back(binding.gpuIndex);
        }
        std::vector<GpuInstanceIndex> instanceIndices;
        std::vector<GpuMaterialIndex> materialIndices;
        instanceIndices.reserve(layout.instances().size());
        materialIndices.reserve(layout.instances().size());
        for (const GpuInstanceBinding& binding : layout.instances()) {
            instanceIndices.push_back(binding.instanceIndex);
            materialIndices.push_back(binding.materialIndex);
        }

        std::vector<GpuMeshData> meshRecords(tableSize<GpuMeshIndex>(meshIndices));
        std::vector<GpuInstanceData> instanceRecords(tableSize<GpuInstanceIndex>(instanceIndices));
        std::vector<GpuMaterialData> materialRecords(tableSize<GpuMaterialIndex>(materialIndices));
        std::vector<GpuDirectionalLightData> lightRecords(1);
        lightRecords[sunLightGpuIndex.value()] = packLight(snapshot.environment().sun);

        version->geometry.reserve(layout.meshes().size());
        version->blasGeometry.reserve(layout.meshes().size());
        for (const GpuMeshBinding& binding : layout.meshes()) {
            if (binding.snapshotMeshIndex >= snapshot.meshes().size()) {
                throw std::logic_error("GPU mesh binding references an invalid snapshot mesh.");
            }
            const assets::Mesh& source = snapshot.meshes()[binding.snapshotMeshIndex].mesh;
            if (source.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
                source.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("GPU scene geometry exceeds the 32-bit shader record range.");
            }

            std::vector<GpuPackedVertex> vertices;
            vertices.reserve(source.vertices.size());
            for (const assets::Vertex& vertex : source.vertices) {
                vertices.push_back(GpuPackedVertex{glm::vec4{vertex.position, 0.0F}, glm::vec4{vertex.normal, 0.0F}});
            }
            std::vector<std::uint32_t> indices = source.indices;
            if (vertices.empty()) {
                vertices.emplace_back();
            }
            if (indices.empty()) {
                indices.push_back(0U);
            }

            nvrhi::BufferDesc vertexDesc =
                structuredBufferDesc(vertices.size() * sizeof(GpuPackedVertex), sizeof(GpuPackedVertex),
                                     "GpuScene.Vertices." + std::to_string(binding.gpuIndex.value()));
            vertexDesc.isVertexBuffer = true;
            vertexDesc.isAccelStructBuildInput = config_.rayTracingEnabled;
            nvrhi::BufferDesc indexDesc =
                structuredBufferDesc(indices.size() * sizeof(std::uint32_t), sizeof(std::uint32_t),
                                     "GpuScene.Indices." + std::to_string(binding.gpuIndex.value()));
            indexDesc.isIndexBuffer = true;
            indexDesc.isAccelStructBuildInput = config_.rayTracingEnabled;

            nvrhi::BufferHandle vertexBuffer = backend_.createBuffer(vertexDesc);
            nvrhi::BufferHandle indexBuffer = backend_.createBuffer(indexDesc);
            if (!vertexBuffer || !indexBuffer) {
                throw std::runtime_error("Failed to create GPU scene geometry buffers.");
            }
            version->uploads.push_back(BufferUpload{vertexBuffer, asBytes<GpuPackedVertex>(vertices)});
            version->uploads.push_back(BufferUpload{indexBuffer, asBytes<std::uint32_t>(indices)});

            GpuGeometryDescriptor descriptor{
                .meshIndex = binding.gpuIndex,
                .vertices = vertexBuffer,
                .indices = indexBuffer,
                .vertexCount = static_cast<std::uint32_t>(source.vertices.size()),
                .indexCount = static_cast<std::uint32_t>(source.indices.size()),
                .blas = nullptr,
            };
            meshRecords[binding.gpuIndex.value()].counts =
                glm::uvec4{descriptor.vertexCount, descriptor.indexCount, 0U, 0U};

            if (config_.rayTracingEnabled && descriptor.vertexCount != 0U && descriptor.indexCount >= 3U) {
                nvrhi::rt::GeometryTriangles triangles;
                triangles.vertexBuffer = vertexBuffer;
                triangles.indexBuffer = indexBuffer;
                triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
                triangles.indexFormat = nvrhi::Format::R32_UINT;
                triangles.vertexStride = sizeof(GpuPackedVertex);
                triangles.vertexCount = descriptor.vertexCount;
                triangles.indexCount = descriptor.indexCount;

                nvrhi::rt::GeometryDesc geometry;
                geometry.setTriangles(triangles).setFlags(nvrhi::rt::GeometryFlags::Opaque);
                nvrhi::rt::AccelStructDesc blasDesc;
                blasDesc.addBottomLevelGeometry(geometry)
                    .setBuildFlags(nvrhi::rt::AccelStructBuildFlags::AllowUpdate |
                                   nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
                    .setDebugName("GpuScene.BLAS." + std::to_string(binding.gpuIndex.value()));
                descriptor.blas = backend_.createAccelerationStructure(blasDesc);
                if (!descriptor.blas) {
                    throw std::runtime_error("Failed to create a GPU scene BLAS.");
                }
                version->blasGeometry.push_back(geometry);
            } else {
                version->blasGeometry.emplace_back();
            }
            version->geometry.push_back(std::move(descriptor));
        }

        for (const GpuInstanceBinding& binding : layout.instances()) {
            if (binding.snapshotInstanceIndex >= snapshot.instances().size()) {
                throw std::logic_error("GPU instance binding references an invalid snapshot instance.");
            }
            const world::RenderWorldInstance& source = snapshot.instances()[binding.snapshotInstanceIndex];
            const glm::mat4 model = source.model.transform.matrix();
            const auto geometry = std::ranges::find_if(version->geometry, [&](const auto& candidate) {
                return candidate.meshIndex == binding.meshIndex;
            });
            if (geometry == version->geometry.end()) {
                throw std::logic_error("GPU instance binding references a missing physical geometry descriptor.");
            }
            const auto geometryDescriptorIndex =
                static_cast<std::uint32_t>(std::distance(version->geometry.begin(), geometry));
            instanceRecords[binding.instanceIndex.value()] = GpuInstanceData{
                .model = model,
                .normalMatrix = makeNormalMatrix(model),
                .metadata = glm::uvec4{binding.materialIndex.value(), 0U, 0U, geometryDescriptorIndex},
            };
            materialRecords[binding.materialIndex.value()] = packGpuMaterial(source.model.material, 0U);

            if (config_.rayTracingEnabled) {
                if (geometry->blas) {
                    nvrhi::rt::InstanceDesc instance;
                    copyAffineTransform(instance, model);
                    instance.setInstanceID(binding.instanceIndex.value())
                        .setInstanceMask(0xFFU)
                        .setInstanceContributionToHitGroupIndex(0U)
                        .setFlags(nvrhi::rt::InstanceFlags::TriangleFrontCounterclockwise)
                        .setBLAS(geometry->blas);
                    version->tlasInstances.push_back(instance);
                }
            }
        }

        const auto createTable = [&](auto& values, const char* name) {
            using Value = typename std::remove_reference_t<decltype(values)>::value_type;
            nvrhi::BufferHandle buffer =
                backend_.createBuffer(structuredBufferDesc(values.size() * sizeof(Value), sizeof(Value), name));
            if (!buffer) {
                throw std::runtime_error("Failed to create a GPU scene record buffer.");
            }
            version->uploads.push_back(BufferUpload{buffer, asBytes<Value>(values)});
            return buffer;
        };
        version->descriptors.meshes = createTable(meshRecords, "GpuScene.MeshRecords");
        version->descriptors.instances = createTable(instanceRecords, "GpuScene.InstanceRecords");
        version->descriptors.materials = createTable(materialRecords, "GpuScene.MaterialRecords");
        version->descriptors.lights = createTable(lightRecords, "GpuScene.LightRecords");

        if (config_.rayTracingEnabled) {
            nvrhi::rt::AccelStructDesc tlasDesc;
            tlasDesc.setTopLevelMaxInstances(std::max<std::size_t>(version->tlasInstances.size(), 1U))
                .setBuildFlags(nvrhi::rt::AccelStructBuildFlags::AllowUpdate |
                               nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
                .setDebugName("GpuScene.TLAS");
            version->descriptors.tlas = backend_.createAccelerationStructure(tlasDesc);
            if (!version->descriptors.tlas) {
                throw std::runtime_error("Failed to create the GPU scene TLAS.");
            }
        }

        std::vector<FrameGraphResourceHandle> uploadResources;
        std::vector<FrameGraphResourceHandle> accelerationStructureInputs;
        uploadResources.reserve(version->uploads.size());
        accelerationStructureInputs.reserve(version->geometry.size() * 2U);
        for (std::size_t index = 0; index < version->uploads.size(); ++index) {
            const BufferUpload& upload = version->uploads[index];
            const FrameGraphResourceHandle resource =
                frameGraph.importBuffer("gpu-scene-upload-" + std::to_string(index),
                                        FrameGraphBufferDesc{.size = upload.buffer->getDesc().byteSize,
                                                             .buffer = upload.buffer,
                                                             .initialState = nvrhi::ResourceStates::Common,
                                                             .finalState = nvrhi::ResourceStates::ShaderResource});
            uploadResources.push_back(resource);
            if (index < version->geometry.size() * 2U) {
                accelerationStructureInputs.push_back(resource);
            }
        }

        GpuScenePreparedUpdate ticket;
        ticket.owner_ = this;
        ticket.frameSlot_ = frameSlot;
        ticket.serial_ = nextSerial_++;
        ticket.generation_ = version->descriptors.generation;
        ticket.uploadPass_ = frameGraph.addPass(
            "gpu-scene-upload", FrameGraphPassType::Transfer,
            [uploadResources](FrameGraphBuilder& builder) {
                for (const FrameGraphResourceHandle resource : uploadResources) {
                    builder.write(resource, nvrhi::ResourceStates::CopyDest);
                }
            },
            [this, version](const FrameGraphContext& context) {
                for (const BufferUpload& upload : version->uploads) {
                    backend_.writeBuffer(context.commandList, upload.buffer, upload.bytes.data(), upload.bytes.size());
                }
            });
        ticket.bufferResources_ = uploadResources;
        ticket.geometryResources_.reserve(version->geometry.size());
        for (std::size_t index = 0; index < version->geometry.size(); ++index) {
            ticket.geometryResources_.push_back(GpuGeometryFrameGraphResources{
                .meshIndex = version->geometry[index].meshIndex,
                .vertices = uploadResources[index * 2U],
                .indices = uploadResources[index * 2U + 1U],
            });
        }
        const std::size_t tableResourceOffset = version->geometry.size() * 2U;
        ticket.meshRecordsResource_ = uploadResources[tableResourceOffset];
        ticket.instanceRecordsResource_ = uploadResources[tableResourceOffset + 1U];
        ticket.materialRecordsResource_ = uploadResources[tableResourceOffset + 2U];
        ticket.lightRecordsResource_ = uploadResources[tableResourceOffset + 3U];

        if (config_.rayTracingEnabled) {
            std::vector<FrameGraphResourceHandle> blasResources;
            blasResources.reserve(version->geometry.size());
            for (std::size_t index = 0; index < version->geometry.size(); ++index) {
                if (!version->geometry[index].blas) {
                    blasResources.push_back({});
                    continue;
                }
                blasResources.push_back(frameGraph.importAccelerationStructure(
                    "gpu-scene-blas-" + std::to_string(index),
                    FrameGraphAccelerationStructureDesc{.accelerationStructure = version->geometry[index].blas,
                                                        .initialState = nvrhi::ResourceStates::Common,
                                                        .finalState = nvrhi::ResourceStates::AccelStructRead}));
            }
            const FrameGraphResourceHandle tlasResource = frameGraph.importAccelerationStructure(
                "gpu-scene-tlas",
                FrameGraphAccelerationStructureDesc{.accelerationStructure = version->descriptors.tlas,
                                                    .initialState = nvrhi::ResourceStates::Common,
                                                    .finalState = nvrhi::ResourceStates::AccelStructRead});
            ticket.topLevelAccelerationStructureResource_ = tlasResource;

            const FrameGraphPassHandle bottomLevelBuildPass = frameGraph.addPass(
                "gpu-scene-build-bottom-level-acceleration-structures", FrameGraphPassType::Compute,
                [accelerationStructureInputs, blasResources](FrameGraphBuilder& builder) {
                    for (const FrameGraphResourceHandle resource : accelerationStructureInputs) {
                        builder.read(resource, nvrhi::ResourceStates::AccelStructBuildInput);
                    }
                    for (const FrameGraphResourceHandle resource : blasResources) {
                        if (resource.isValid()) {
                            builder.writeAccelerationStructure(resource);
                        }
                    }
                },
                [this, version](const FrameGraphContext& context) {
                    const auto flags = nvrhi::rt::AccelStructBuildFlags::AllowUpdate |
                                       nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
                    for (std::size_t index = 0; index < version->geometry.size(); ++index) {
                        if (version->geometry[index].blas) {
                            backend_.buildBottomLevel(context.commandList, version->geometry[index].blas,
                                                      version->blasGeometry[index], flags);
                        }
                    }
                });

            ticket.accelerationStructurePass_ = frameGraph.addPass(
                "gpu-scene-build-top-level-acceleration-structure", FrameGraphPassType::Compute,
                [bottomLevelBuildPass, blasResources, tlasResource](FrameGraphBuilder& builder) {
                    builder.dependsOn(bottomLevelBuildPass);
                    // TLAS 构建会读取 BLAS 的设备地址和构建结果。单独声明 read 可强制 FrameGraph 在两个
                    // build pass 之间提交 AccelStructWrite -> AccelStructRead 内存屏障。
                    for (const FrameGraphResourceHandle resource : blasResources) {
                        if (resource.isValid()) {
                            builder.readAccelerationStructure(resource);
                        }
                    }
                    builder.writeAccelerationStructure(tlasResource);
                },
                [this, version](const FrameGraphContext& context) {
                    const auto flags = nvrhi::rt::AccelStructBuildFlags::AllowUpdate |
                                       nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
                    backend_.buildTopLevel(context.commandList, version->descriptors.tlas, version->tlasInstances,
                                           flags);
                });
        }

        pending_[slot] =
            std::make_shared<PendingVersion>(PendingVersion{ticket.serial_, version, std::move(version)});
        return ticket;
    }

    void GpuSceneResources::finishUpdate(const GpuScenePreparedUpdate& update, bool submitted) {
        (void)validatePending(update);
        const std::size_t slot = validateSlot(update.frameSlot_);
        if (submitted) {
            if (const std::shared_ptr<SlotVersion>& candidate = pending_[slot]->writableCandidate) {
                // 命令已完成录制并成功提交，新候选无需继续持有 CPU upload 与 AS build 描述副本。
                std::vector<BufferUpload>{}.swap(candidate->uploads);
                std::vector<nvrhi::rt::GeometryDesc>{}.swap(candidate->blasGeometry);
                std::vector<nvrhi::rt::InstanceDesc>{}.swap(candidate->tlasInstances);
            }
            slots_[slot] = std::move(pending_[slot]->version);
        }
        pending_[slot].reset();
    }

    const GpuSceneResources::PendingVersion&
    GpuSceneResources::validatePending(const GpuScenePreparedUpdate& update) const {
        if (update.owner_ != this || !update.isValid()) {
            throw std::invalid_argument("GPU scene update ticket belongs to another resource set.");
        }
        const std::size_t slot = validateSlot(update.frameSlot_);
        if (!pending_[slot] || pending_[slot]->serial != update.serial_) {
            throw std::logic_error("GPU scene update ticket is stale or already finished.");
        }
        return *pending_[slot];
    }

    GpuSceneDescriptors GpuSceneResources::candidateDescriptors(const GpuScenePreparedUpdate& update) const {
        return validatePending(update).version->descriptors;
    }

    std::span<const GpuGeometryDescriptor>
    GpuSceneResources::candidateGeometry(const GpuScenePreparedUpdate& update) const {
        return validatePending(update).version->geometry;
    }

    GpuSceneDescriptors GpuSceneResources::descriptors(core::FrameSlotIndex frameSlot) const {
        const std::shared_ptr<const SlotVersion>& version = slots_[validateSlot(frameSlot)];
        return version ? version->descriptors : GpuSceneDescriptors{};
    }

    std::span<const GpuGeometryDescriptor> GpuSceneResources::geometry(core::FrameSlotIndex frameSlot) const {
        const std::shared_ptr<const SlotVersion>& version = slots_[validateSlot(frameSlot)];
        return version ? std::span<const GpuGeometryDescriptor>{version->geometry}
                       : std::span<const GpuGeometryDescriptor>{};
    }

    bool GpuSceneResources::rayTracingEnabled() const noexcept {
        return config_.rayTracingEnabled;
    }

    void GpuSceneResources::clear() noexcept {
        std::ranges::fill(slots_, nullptr);
        std::ranges::fill(pending_, nullptr);
    }

} // namespace lumin::render::gpu

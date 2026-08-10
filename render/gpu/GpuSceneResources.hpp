#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "render/FrameGraph.hpp"
#include "render/gpu/GpuScene.hpp"

namespace lumin::render::gpu {

    class GpuSceneResources;

    /** ray tracing 与 raster 共用的 32 字节顶点记录。 */
    struct alignas(16) GpuPackedVertex {
        /// 世界变换前的 object-space 位置；w 为对齐填充。
        glm::vec4 position;
        /// object-space 法线；w 为对齐填充。
        glm::vec4 normal;
    };

    /** raster、ray tracing、SHARC 与 NRD 共用的实例记录。 */
    struct alignas(16) GpuInstanceData {
        /// object-space 到 world-space 的模型矩阵。
        glm::mat4 model{1.0F};
        /// 法线变换矩阵；平移分量固定为零。
        glm::mat4 normalMatrix{1.0F};
        /// x=materialIndex，y=firstIndex，z=vertexOffset，w=对应 `geometry(slot)` 的稠密 descriptor 索引。
        glm::uvec4 metadata{0U};
    };

    /** 描述单个稳定 mesh slot 所使用的 geometry 范围。 */
    struct alignas(16) GpuMeshData {
        /// x=vertexCount，y=indexCount，z=firstIndex，w=vertexOffset。
        glm::uvec4 counts{0U};
    };

    /** 场景方向光的共享 GPU 记录。 */
    struct alignas(16) GpuDirectionalLightData {
        /// xyz 为光线传播方向，w 为 illuminanceLux。
        glm::vec4 directionIlluminance{0.0F};
        /// rgb 为线性光色，w 为 castsShadows 标志。
        glm::vec4 colorCastsShadows{0.0F};
    };

    static_assert(sizeof(GpuPackedVertex) == 32);
    static_assert(sizeof(GpuInstanceData) == 144);
    static_assert(sizeof(GpuMeshData) == 16);
    static_assert(sizeof(GpuDirectionalLightData) == 32);

    /** 每个物理 geometry 版本对外暴露的只读资源描述。 */
    struct GpuGeometryDescriptor {
        /// 稳定 GPU mesh 索引。
        GpuMeshIndex meshIndex;
        /// 32 字节结构化顶点 buffer。
        nvrhi::BufferHandle vertices;
        /// `uint32_t` 结构化索引 buffer。
        nvrhi::BufferHandle indices;
        /// 顶点数量。
        std::uint32_t vertexCount = 0;
        /// 索引数量。
        std::uint32_t indexCount = 0;
        /// RT 开启时对应的 BLAS；fallback 模式为空。
        nvrhi::rt::AccelStructHandle blas;
    };

    /** 某一帧槽已成功提交的共享 GPU Scene descriptor 集合。 */
    struct GpuSceneDescriptors {
        /// mesh record structured buffer。
        nvrhi::BufferHandle meshes;
        /// instance record structured buffer。
        nvrhi::BufferHandle instances;
        /// 统一材质 structured buffer。
        nvrhi::BufferHandle materials;
        /// 方向光 structured buffer。
        nvrhi::BufferHandle lights;
        /// RT 开启时的当前 TLAS；fallback 模式为空。
        nvrhi::rt::AccelStructHandle tlas;
        /// 此物理版本是否包含可追踪 AS。
        bool rayTracingEnabled = false;
        /// 此版本对应的 GPU 可见内容 generation；稳定帧不会推进该值。
        std::uint64_t generation = 0;
    };

    /** GPU Scene 物理资源层配置。 */
    struct GpuSceneResourceConfig {
        /// 同时在 flight 的帧槽数量。
        std::uint32_t frameSlotCount = 2;
        /// 仅当 `VulkanContext::rayTracingDecision().enabled()` 时设为 `true`。
        bool rayTracingEnabled = false;
    };

    /** 单个 bindless geometry 对应的 FrameGraph buffer handles。 */
    struct GpuGeometryFrameGraphResources {
        /// 与 `candidateGeometry()` 同位置记录对应的稳定 mesh 索引。
        GpuMeshIndex meshIndex;
        /// packed vertex buffer resource。
        FrameGraphResourceHandle vertices;
        /// `uint32_t` index buffer resource。
        FrameGraphResourceHandle indices;
    };

    /**
     * GPU Scene 使用的窄 NvRHI 后端边界。
     *
     * 该接口便于在不创建 Vulkan device 的测试中验证资源与 pass 契约。生产实现仍只调用 NvRHI，
     * 不向资源层暴露任何原生 Vulkan handle。
     */
    class GpuSceneBackend {
    public:
        virtual ~GpuSceneBackend() = default;

        /// 创建 buffer；失败时返回空 handle。
        [[nodiscard]] virtual nvrhi::BufferHandle createBuffer(const nvrhi::BufferDesc& desc) = 0;
        /// 创建 BLAS 或 TLAS；RT fallback 路径绝不会调用。
        [[nodiscard]] virtual nvrhi::rt::AccelStructHandle
        createAccelerationStructure(const nvrhi::rt::AccelStructDesc& desc) = 0;
        /// 在 FrameGraph 已转换为 `CopyDest` 后录制 buffer 写入。
        virtual void writeBuffer(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, const void* data,
                                 std::size_t size) = 0;
        /// 在 FrameGraph 已声明 build input 与 AS write 后录制 BLAS build。
        virtual void buildBottomLevel(nvrhi::ICommandList* commandList, nvrhi::rt::IAccelStruct* blas,
                                      const nvrhi::rt::GeometryDesc& geometry,
                                      nvrhi::rt::AccelStructBuildFlags flags) = 0;
        /// 在 FrameGraph 已声明所有 BLAS read 与 TLAS write 后录制 TLAS build。
        virtual void buildTopLevel(nvrhi::ICommandList* commandList, nvrhi::rt::IAccelStruct* tlas,
                                   std::span<const nvrhi::rt::InstanceDesc> instances,
                                   nvrhi::rt::AccelStructBuildFlags flags) = 0;
    };

    /** 直接转发到 NvRHI device/command-list 的生产 GPU Scene 后端。 */
    class NvrhiGpuSceneBackend final : public GpuSceneBackend {
    public:
        /// 构造 NvRHI 后端边界；device 生命周期必须覆盖本对象。
        explicit NvrhiGpuSceneBackend(nvrhi::IDevice& device) noexcept;

        [[nodiscard]] nvrhi::BufferHandle createBuffer(const nvrhi::BufferDesc& desc) override;
        [[nodiscard]] nvrhi::rt::AccelStructHandle
        createAccelerationStructure(const nvrhi::rt::AccelStructDesc& desc) override;
        void writeBuffer(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, const void* data,
                         std::size_t size) override;
        void buildBottomLevel(nvrhi::ICommandList* commandList, nvrhi::rt::IAccelStruct* blas,
                              const nvrhi::rt::GeometryDesc& geometry, nvrhi::rt::AccelStructBuildFlags flags) override;
        void buildTopLevel(nvrhi::ICommandList* commandList, nvrhi::rt::IAccelStruct* tlas,
                           std::span<const nvrhi::rt::InstanceDesc> instances,
                           nvrhi::rt::AccelStructBuildFlags flags) override;

    private:
        nvrhi::IDevice& device_;
    };

    /** 一次已经写入 FrameGraph、尚未确认 submit 的物理更新票据。 */
    class GpuScenePreparedUpdate {
    public:
        /// 返回票据是否关联有效候选版本。
        [[nodiscard]] bool isValid() const noexcept;
        /// 返回目标帧槽。
        [[nodiscard]] core::FrameSlotIndex frameSlot() const noexcept;
        /// 返回候选 generation。
        [[nodiscard]] std::uint64_t generation() const noexcept;
        /// 返回上传 pass。
        [[nodiscard]] FrameGraphPassHandle uploadPass() const noexcept;
        /// 返回 AS build pass；fallback 模式下无效。
        [[nodiscard]] FrameGraphPassHandle accelerationStructurePass() const noexcept;
        /// 返回候选版本所有 buffer 的 FrameGraph resource handle，供同帧消费者声明 SRV read。
        [[nodiscard]] std::span<const FrameGraphResourceHandle> bufferResources() const noexcept;
        /// 返回 mesh record table resource。
        [[nodiscard]] FrameGraphResourceHandle meshRecordsResource() const noexcept;
        /// 返回 instance record table resource。
        [[nodiscard]] FrameGraphResourceHandle instanceRecordsResource() const noexcept;
        /// 返回 material record table resource。
        [[nodiscard]] FrameGraphResourceHandle materialRecordsResource() const noexcept;
        /// 返回 light record table resource。
        [[nodiscard]] FrameGraphResourceHandle lightRecordsResource() const noexcept;
        /// 返回与 `candidateGeometry()` 顺序一致的 vertex/index resources。
        [[nodiscard]] std::span<const GpuGeometryFrameGraphResources> geometryResources() const noexcept;
        /// 返回候选 TLAS 的 FrameGraph resource handle；fallback 模式下无效。trace pass 必须复用此 handle。
        [[nodiscard]] FrameGraphResourceHandle tlasResource() const noexcept;
        /// `tlasResource()` 的兼容长名称。
        [[nodiscard]] FrameGraphResourceHandle topLevelAccelerationStructureResource() const noexcept;

    private:
        friend class GpuSceneResources;
        const GpuSceneResources* owner_ = nullptr;
        core::FrameSlotIndex frameSlot_;
        std::uint64_t serial_ = 0;
        std::uint64_t generation_ = 0;
        FrameGraphPassHandle uploadPass_;
        FrameGraphPassHandle accelerationStructurePass_;
        std::vector<FrameGraphResourceHandle> bufferResources_;
        FrameGraphResourceHandle meshRecordsResource_;
        FrameGraphResourceHandle instanceRecordsResource_;
        FrameGraphResourceHandle materialRecordsResource_;
        FrameGraphResourceHandle lightRecordsResource_;
        std::vector<GpuGeometryFrameGraphResources> geometryResources_;
        FrameGraphResourceHandle topLevelAccelerationStructureResource_;
    };

    /**
     * 将 `GpuSceneUpdatePlan` 物化为 per-frame-slot buffer 与可选 BLAS/TLAS。
     *
     * 每次更新先创建独立候选版本并由 FrameGraph 录制上传/AS build。只有命令成功 submit 后才能调用
     * `finishUpdate(..., true)` 发布；失败时调用 `finishUpdate(..., false)` 丢弃。其他帧槽及同一槽的已发布
     * 版本在此之前均保持存活，因而不会覆盖 in-flight 资源。
     */
    class GpuSceneResources {
    public:
        /// 构造物理资源层。
        GpuSceneResources(GpuSceneBackend& backend, GpuSceneResourceConfig config);
        ~GpuSceneResources();

        GpuSceneResources(const GpuSceneResources&) = delete;
        GpuSceneResources& operator=(const GpuSceneResources&) = delete;

        /**
         * 创建候选物理版本并把 upload/AS pass 注册到 FrameGraph。
         *
         * @throws std::logic_error 对应 slot fence 尚未等待，或该 slot 已有未完成票据。
         * @throws std::out_of_range 帧槽超出配置范围。
         * @throws std::runtime_error NvRHI 资源创建失败。
         *
         * 当前物理版本采用完整 copy-on-write；planner 的 `Refit` 决策会安全降级为新 BLAS full build，
         * 待后端具备跨帧槽 update 兼容性证明后再启用原地 `PerformUpdate`。
         */
        [[nodiscard]] GpuScenePreparedUpdate recordUpdate(FrameGraph& frameGraph, const GpuSceneUpdatePlan& plan,
                                                          core::FrameSlotIndex frameSlot, bool frameSlotFenceWaited);

        /**
         * 在命令提交结果已知后完成票据。
         *
         * `submitted=true` 发布候选版本；`false` 丢弃候选版本且保持已发布 descriptor 不变。
         */
        void finishUpdate(const GpuScenePreparedUpdate& update, bool submitted);

        /**
         * 返回尚未提交候选版本的 descriptor。
         *
         * RT trace pass 可在同一 FrameGraph 帧内读取这些 owning handles，并显式声明对 candidate TLAS 的 read；
         * 调用 `finishUpdate` 后票据失效，再次查询将抛出异常。
         */
        [[nodiscard]] GpuSceneDescriptors candidateDescriptors(const GpuScenePreparedUpdate& update) const;

        /**
         * 返回尚未提交候选版本的 bindless geometry descriptor 顺序。
         *
         * 数组下标与候选 `GpuInstanceData::metadata.w` 完全一致；span 仅在 `finishUpdate` 前有效。
         */
        [[nodiscard]] std::span<const GpuGeometryDescriptor>
        candidateGeometry(const GpuScenePreparedUpdate& update) const;

        /// 返回指定帧槽已发布的 descriptor；尚未成功提交时返回空记录。
        [[nodiscard]] GpuSceneDescriptors descriptors(core::FrameSlotIndex frameSlot) const;
        /**
         * 返回指定帧槽已发布的 geometry descriptor。
         *
         * 数组下标是 `GpuInstanceData::metadata.w` 使用的 bindless geometry descriptor 索引；下一次成功发布
         * 该 slot 前 span 保持有效。
         */
        [[nodiscard]] std::span<const GpuGeometryDescriptor> geometry(core::FrameSlotIndex frameSlot) const;
        /// 返回是否启用 AS 资源创建。
        [[nodiscard]] bool rayTracingEnabled() const noexcept;
        /// 清除全部版本；调用方必须先等待 device idle。
        void clear() noexcept;

    private:
        struct SlotVersion;
        struct PendingVersion;

        [[nodiscard]] std::size_t validateSlot(core::FrameSlotIndex frameSlot) const;
        [[nodiscard]] const PendingVersion& validatePending(const GpuScenePreparedUpdate& update) const;

        GpuSceneBackend& backend_;
        GpuSceneResourceConfig config_;
        std::uint64_t nextSerial_ = 1;
        std::vector<std::shared_ptr<const SlotVersion>> slots_;
        std::vector<std::shared_ptr<PendingVersion>> pending_;
    };

} // namespace lumin::render::gpu

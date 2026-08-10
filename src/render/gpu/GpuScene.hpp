#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "render/core/FrameIdentity.hpp"
#include "render/gpu/GpuMaterial.hpp"
#include "render/world/RenderWorld.hpp"

namespace lumin::render::gpu {

    /**
     * 将 GPU 场景表索引封装为不可隐式互换的强类型。
     *
     * `Tag` 只用于区分 mesh、instance、material 和 light 表；默认构造值无效。
     */
    template <typename Tag> class GpuSceneIndex final {
    public:
        /// 底层整数类型。
        using ValueType = std::uint32_t;

        /// 表示无效 GPU 表索引的保留值。
        static constexpr ValueType invalidValue = std::numeric_limits<ValueType>::max();

        /// 构造无效索引。
        constexpr GpuSceneIndex() noexcept = default;

        /// 从显式整数构造索引。
        explicit constexpr GpuSceneIndex(ValueType value) noexcept : value_(value) {
        }

        /// 返回底层整数值；调用方应先检查 `isValid()`。
        [[nodiscard]] constexpr ValueType value() const noexcept {
            return value_;
        }

        /// 返回索引是否有效。
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return value_ != invalidValue;
        }

        /// 允许在条件表达式中显式检查有效性。
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return isValid();
        }

        friend constexpr auto operator<=>(const GpuSceneIndex&, const GpuSceneIndex&) noexcept = default;

    private:
        ValueType value_ = invalidValue;
    };

    /// GPU mesh 表中的稳定索引类型。
    using GpuMeshIndex = GpuSceneIndex<struct GpuMeshIndexTag>;

    /// GPU instance 表中的稳定索引类型。
    using GpuInstanceIndex = GpuSceneIndex<struct GpuInstanceIndexTag>;

    /// GPU material 表中的稳定索引类型。
    using GpuMaterialIndex = GpuSceneIndex<struct GpuMaterialIndexTag>;

    /// GPU light 表中的稳定索引类型。
    using GpuLightIndex = GpuSceneIndex<struct GpuLightIndexTag>;

    /**
     * 由 `scene::ModelHandle` 派生的稳定渲染实例身份。
     *
     * slot 被复用时 generation 会改变，因此删除旧模型后创建的新模型不会继承旧身份。
     */
    class RenderInstanceId final {
    public:
        /// 构造无效实例身份。
        constexpr RenderInstanceId() noexcept = default;

        /// 从稳定场景模型句柄构造身份。
        explicit constexpr RenderInstanceId(scene::ModelHandle handle) noexcept
            : slot_(handle.index), generation_(handle.generation) {
        }

        /// 返回身份是否来自有效模型句柄。
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return slot_ != std::numeric_limits<std::uint32_t>::max() && generation_ != 0;
        }

        /// 返回源模型 slot。
        [[nodiscard]] constexpr std::uint32_t slot() const noexcept {
            return slot_;
        }

        /// 返回源模型 generation。
        [[nodiscard]] constexpr std::uint32_t generation() const noexcept {
            return generation_;
        }

        /// 还原用于查找快照实例的场景模型句柄。
        [[nodiscard]] constexpr scene::ModelHandle modelHandle() const noexcept {
            return scene::ModelHandle{slot_, generation_};
        }

        friend constexpr auto operator<=>(const RenderInstanceId&, const RenderInstanceId&) noexcept = default;

    private:
        std::uint32_t slot_ = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation_ = 0;
    };

    /**
     * 将稳定实例身份映射到共享 GPU material 表索引。
     *
     * material 表以 `ModelHandle` 的 slot 稀疏寻址；generation 只用于区分逻辑身份。slot 被复用时，
     * frame-slot 物理资源的 copy-on-write 版本负责隔离仍在 flight 的旧 generation。
     */
    [[nodiscard]] constexpr GpuMaterialIndex materialIndexFor(RenderInstanceId id) noexcept {
        return id.isValid() ? GpuMaterialIndex{id.slot()} : GpuMaterialIndex{};
    }

    /// 太阳方向光固定使用的 GPU light 索引。
    inline constexpr GpuLightIndex sunLightGpuIndex{0};

    class GpuSceneUpdatePlanner;

    /// 将源 mesh 身份映射到当前快照和稳定 GPU 索引。
    struct GpuMeshBinding {
        /// 源场景 mesh 的稳定句柄。
        scene::MeshHandle sourceHandle;
        /// GPU mesh 表索引。
        GpuMeshIndex gpuIndex;
        /// 当前快照 `meshes()` 中的索引。
        world::RenderMeshIndex snapshotMeshIndex = 0;
    };

    /// 将稳定实例身份映射到 GPU instance、mesh 和 material 表。
    struct GpuInstanceBinding {
        /// 稳定渲染实例身份。
        RenderInstanceId instanceId;
        /// GPU instance 表索引。
        GpuInstanceIndex instanceIndex;
        /// 当前实例引用的 GPU mesh 索引。
        GpuMeshIndex meshIndex;
        /// 当前实例独占的稳定 GPU material 索引。
        GpuMaterialIndex materialIndex;
        /// 当前快照 `instances()` 中的索引。
        std::uint32_t snapshotInstanceIndex = 0;
    };

    /** 当前 immutable snapshot 对应的活动 GPU 索引布局。 */
    class GpuSceneLayout {
    public:
        /// 返回当前活动 mesh 映射。
        [[nodiscard]] const std::vector<GpuMeshBinding>& meshes() const noexcept;

        /// 返回当前活动 instance 映射。
        [[nodiscard]] const std::vector<GpuInstanceBinding>& instances() const noexcept;

        /// 按源句柄查找活动 mesh 映射；不存在时返回 `nullptr`。
        [[nodiscard]] const GpuMeshBinding* findMesh(scene::MeshHandle handle) const noexcept;

        /// 按稳定身份查找活动 instance 映射；不存在时返回 `nullptr`。
        [[nodiscard]] const GpuInstanceBinding* findInstance(RenderInstanceId id) const noexcept;

        /// 返回太阳方向光的固定 GPU 索引。
        [[nodiscard]] constexpr GpuLightIndex sunLight() const noexcept {
            return sunLightGpuIndex;
        }

    private:
        friend class GpuSceneUpdatePlanner;

        std::vector<GpuMeshBinding> meshes_;
        std::vector<GpuInstanceBinding> instances_;
    };

    /** 描述一份 mesh 数据需要上传到哪个稳定 GPU slot。 */
    struct GpuGeometryUpload {
        /// 目标 GPU mesh 索引。
        GpuMeshIndex meshIndex;
        /// 目标快照中的 mesh 索引。
        world::RenderMeshIndex snapshotMeshIndex = 0;
        /// 顶点或索引数量改变时为 `true`，后端必须重新分配对应 buffer 范围。
        bool requiresAllocationResize = false;
        /**
         * 同一逻辑 mesh slot 是否仍有已提交物理版本。
         *
         * 为 `true` 时不得原地覆盖旧 buffer；后端应创建新版本，并在所有引用旧版本的 fence 完成后回收。
         */
        bool replacesLiveAllocation = false;
    };

    /** 实例数据局部更新类别。 */
    enum class GpuInstancePatchMask : std::uint8_t {
        /// 无局部更新。
        None = 0,
        /// world transform 发生变化。
        Transform = 1U << 0U,
        /// material 参数或纹理选择参数发生变化。
        Material = 1U << 1U,
    };

    /// 合并两个实例局部更新类别。
    [[nodiscard]] constexpr GpuInstancePatchMask operator|(GpuInstancePatchMask left,
                                                           GpuInstancePatchMask right) noexcept {
        return static_cast<GpuInstancePatchMask>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /// 将更新类别合并到左操作数。
    constexpr GpuInstancePatchMask& operator|=(GpuInstancePatchMask& left, GpuInstancePatchMask right) noexcept {
        left = left | right;
        return left;
    }

    /// 判断局部更新是否包含任意指定类别。
    [[nodiscard]] constexpr bool hasAnyPatch(GpuInstancePatchMask patches, GpuInstancePatchMask categories) noexcept {
        return (static_cast<std::uint8_t>(patches) & static_cast<std::uint8_t>(categories)) != 0;
    }

    /** 拓扑重建时写入 GPU instance/material 表的一条完整记录。 */
    struct GpuInstanceRecord {
        /// 稳定实例身份。
        RenderInstanceId instanceId;
        /// GPU instance 表索引。
        GpuInstanceIndex instanceIndex;
        /// GPU mesh 表索引。
        GpuMeshIndex meshIndex;
        /// GPU material 表索引。
        GpuMaterialIndex materialIndex;
        /// 读取 transform 和 material 的目标快照实例索引。
        std::uint32_t snapshotInstanceIndex = 0;
    };

    /** 拓扑不变时写入 GPU instance/material 表的一条局部更新。 */
    struct GpuInstancePatch {
        /// 稳定实例身份。
        RenderInstanceId instanceId;
        /// GPU instance 表索引。
        GpuInstanceIndex instanceIndex;
        /// GPU material 表索引。
        GpuMaterialIndex materialIndex;
        /// 读取新数据的目标快照实例索引。
        std::uint32_t snapshotInstanceIndex = 0;
        /// 需要写入的字段类别。
        GpuInstancePatchMask fields = GpuInstancePatchMask::None;
    };

    /** 方向光 GPU 数据局部更新。 */
    struct GpuLightPatch {
        /// 要写入的 GPU light 表索引。
        GpuLightIndex lightIndex;
    };

    /** 单个 mesh 的 BLAS 更新策略。 */
    enum class BlasUpdateMode : std::uint8_t {
        /// 几何及已有 BLAS 均可复用。
        Reuse,
        /// 新建或完整重建 BLAS。
        /// 后端创建的 BLAS 必须允许后续 update；已有 in-flight 版本时必须创建新物理版本。
        Build,
        /// 顶点位置变化但拓扑兼容，可对允许更新的 BLAS 执行 refit。
        /// 若 NvRHI 后端无法跨物理版本 refit，可安全降级为 copy-on-write build。
        Refit,
    };

    /** 当前活动 mesh 的 BLAS 决策。 */
    struct BlasUpdateDecision {
        /// 对应 GPU mesh 索引。
        GpuMeshIndex meshIndex;
        /// 读取几何的目标快照 mesh 索引。
        world::RenderMeshIndex snapshotMeshIndex = 0;
        /// 本次采用的 BLAS 策略。
        BlasUpdateMode mode = BlasUpdateMode::Reuse;
    };

    /** 整个场景 TLAS 的更新策略。 */
    enum class TlasUpdateMode : std::uint8_t {
        /// 实例集合、变换及所引用 BLAS 均可复用。
        Reuse,
        /// 实例拓扑改变，需要完整 build。
        /// 后端创建的 TLAS 必须允许后续 update，且不能覆盖仍被 in-flight 帧引用的版本。
        Build,
        /// 拓扑不变但 transform 或 BLAS 地址/内容改变，可执行 update。
        Update,
    };

    /**
     * 后端提交 GPU Scene 更新后的确认信息。
     *
     * `frameSlotFenceWaited` 只能在 `VulkanContext::beginFrame` 已等待对应 slot fence 后设为 `true`；
     * `updateCommandsSubmitted` 只能在包含上传和 AS 更新的命令成功提交后设为 `true`。共享资源发生变化时，
     * 后端还必须通过 copy-on-write、per-slot 版本或延迟回收保护其他 in-flight 帧。
     */
    struct GpuSceneCommitInfo {
        /// 本次允许写入的帧槽。
        core::FrameSlotIndex frameSlot;
        /// 对应帧槽 fence 是否已完成等待。
        bool frameSlotFenceWaited = false;
        /// 本计划的 GPU 更新命令是否已成功提交。
        bool updateCommandsSubmitted = false;
        /// 仍被其他 in-flight 帧引用的物理资源版本是否得到保留。
        bool inFlightResourcesPreserved = false;
    };

    /**
     * 一次 immutable RenderWorld snapshot 到 GPU Scene 的事务式更新计划。
     *
     * 计划拥有 previous/target snapshot 的共享引用，后端可在命令录制期间安全读取其中的数据。
     * 计划不创建任何 GPU 资源，也不会在构建时改变 planner 的已提交状态。
     */
    class GpuSceneUpdatePlan {
    public:
        /// 复制计划及 snapshot 强引用，便于后端为尚未追上该 generation 的帧槽保留重放数据。
        GpuSceneUpdatePlan(const GpuSceneUpdatePlan&) = default;
        GpuSceneUpdatePlan& operator=(const GpuSceneUpdatePlan&) = default;
        GpuSceneUpdatePlan(GpuSceneUpdatePlan&&) noexcept = default;
        GpuSceneUpdatePlan& operator=(GpuSceneUpdatePlan&&) noexcept = default;

        /// 返回计划所基于的 planner 提交代次。
        [[nodiscard]] std::uint64_t baseGeneration() const noexcept;

        /// 返回是否为 planner 的首次 GPU Scene 初始化。
        [[nodiscard]] bool initializesScene() const noexcept;

        /// 返回本次规划采用的场景变化集合；首次规划固定为 `SceneChangeMask::All`。
        [[nodiscard]] world::SceneChangeMask changes() const noexcept;

        /// 返回规划前已提交的 immutable snapshot；首次规划为空。
        [[nodiscard]] const world::RenderWorldSnapshotPtr& previousSnapshot() const noexcept;

        /// 返回本次要发布的 immutable snapshot。
        [[nodiscard]] const world::RenderWorldSnapshotPtr& targetSnapshot() const noexcept;

        /// 返回提交后采用的稳定 GPU 索引布局。
        [[nodiscard]] const GpuSceneLayout& targetLayout() const noexcept;

        /// 返回需要上传或重写的 mesh 数据。
        [[nodiscard]] std::span<const GpuGeometryUpload> geometryUploads() const noexcept;

        /// 返回实例拓扑是否需要完整重建。
        [[nodiscard]] bool rebuildsInstanceTopology() const noexcept;

        /// 返回拓扑完整重建时写入的所有活动实例记录。
        [[nodiscard]] std::span<const GpuInstanceRecord> instanceRecords() const noexcept;

        /// 返回拓扑不变时需要局部写入的实例记录。
        [[nodiscard]] std::span<const GpuInstancePatch> instancePatches() const noexcept;

        /// 返回 material texture descriptor/bindless 映射是否需要重建。
        [[nodiscard]] bool rebuildsMaterialBindings() const noexcept;

        /// 返回需要局部写入的 light 记录。
        [[nodiscard]] std::span<const GpuLightPatch> lightPatches() const noexcept;

        /// 返回每个活动 mesh 的 BLAS build/refit/reuse 决策。
        [[nodiscard]] std::span<const BlasUpdateDecision> blasDecisions() const noexcept;

        /// 返回整个场景的 TLAS build/update/reuse 决策。
        [[nodiscard]] TlasUpdateMode tlasDecision() const noexcept;

        /// 返回离开活动布局、应由后端延迟回收的 GPU mesh 索引。
        [[nodiscard]] std::span<const GpuMeshIndex> retiredMeshes() const noexcept;

        /// 返回离开活动布局、应由后端延迟回收的 GPU instance 索引。
        [[nodiscard]] std::span<const GpuInstanceIndex> retiredInstances() const noexcept;

        /// 返回离开活动布局、应由后端延迟回收的 GPU material 索引。
        [[nodiscard]] std::span<const GpuMaterialIndex> retiredMaterials() const noexcept;

        /// 返回计划是否包含上传、表更新、AS 更新或资源退休操作。
        [[nodiscard]] bool hasGpuWork() const noexcept;

        /**
         * 返回是否会替换或更新可被其他 in-flight 帧引用的 geometry/BLAS/TLAS 物理资源。
         *
         * 返回 `true` 时 commit 必须确认旧物理版本已被保留；单独等待当前 frame-slot fence 不充分。
         */
        [[nodiscard]] bool requiresInFlightResourcePreservation() const noexcept;

    private:
        friend class GpuSceneUpdatePlanner;

        GpuSceneUpdatePlan() = default;

        const GpuSceneUpdatePlanner* owner_ = nullptr;
        std::uint64_t ownerEpoch_ = 0;
        std::uint64_t baseGeneration_ = 0;
        bool initializesScene_ = false;
        world::SceneChangeMask changes_ = world::SceneChangeMask::None;
        world::RenderWorldSnapshotPtr previousSnapshot_;
        world::RenderWorldSnapshotPtr targetSnapshot_;
        GpuSceneLayout targetLayout_;
        std::vector<GpuGeometryUpload> geometryUploads_;
        bool rebuildInstanceTopology_ = false;
        std::vector<GpuInstanceRecord> instanceRecords_;
        std::vector<GpuInstancePatch> instancePatches_;
        bool rebuildMaterialBindings_ = false;
        std::vector<GpuLightPatch> lightPatches_;
        std::vector<BlasUpdateDecision> blasDecisions_;
        TlasUpdateMode tlasDecision_ = TlasUpdateMode::Reuse;
        std::vector<GpuMeshIndex> retiredMeshes_;
        std::vector<GpuInstanceIndex> retiredInstances_;
        std::vector<GpuMaterialIndex> retiredMaterials_;

        std::vector<GpuMeshBinding> meshIndexRegistry_;
        std::vector<GpuInstanceBinding> instanceIndexRegistry_;
        std::uint32_t nextMeshIndex_ = 0;
        std::uint32_t nextInstanceIndex_ = 0;
    };

    /**
     * 为连续 RenderWorld snapshots 分配稳定 GPU 索引并生成事务式更新计划。
     *
     * 同一稳定源身份在暂时离开活动 snapshot 后再次出现时仍复用原索引。mesh/instance 分配器只在 `clear()`
     * 时重置；material index 直接等于模型 slot，slot generation 复用由后端的 frame-slot copy-on-write 物理版本隔离。
     * planner 发布的是逻辑 generation；实际后端必须跟踪每个 frame slot 已应用的 generation，并在使用落后 slot
     * 前重放或合并缺失更新。
     */
    class GpuSceneUpdatePlanner {
    public:
        /// 构造尚未初始化的 planner。
        GpuSceneUpdatePlanner() = default;

        /// planner 不可复制，避免不同实例错误提交彼此的计划。
        GpuSceneUpdatePlanner(const GpuSceneUpdatePlanner&) = delete;
        GpuSceneUpdatePlanner& operator=(const GpuSceneUpdatePlanner&) = delete;
        GpuSceneUpdatePlanner(GpuSceneUpdatePlanner&&) = delete;
        GpuSceneUpdatePlanner& operator=(GpuSceneUpdatePlanner&&) = delete;

        /**
         * 从场景增量生成候选 GPU Scene 更新。
         *
         * 首次规划会忽略增量位并执行完整初始化。该函数不改变 planner，允许失败后重新规划。
         *
         * @throws std::invalid_argument `delta.snapshot` 为空。
         * @throws std::overflow_error 稳定 GPU 索引空间耗尽。
         */
        [[nodiscard]] GpuSceneUpdatePlan plan(const world::SceneDelta& delta) const;

        /**
         * 在 GPU 更新成功提交后发布计划中的 snapshot 和索引布局。
         *
         * 有 GPU 工作时必须提供有效帧槽、已等待 fence 且命令已提交的确认信息。失败或未提交时不得调用；
         * 共享资源变化时还必须保留其他 in-flight 帧引用的旧物理版本。stale plan、其他 planner 生成的 plan
         * 或不满足 fence/submit/lifetime 约束时抛出异常且状态不变。
         */
        void commit(const GpuSceneUpdatePlan& plan, const GpuSceneCommitInfo& info);

        /// 返回当前 GPU 可见内容代次；无 GPU 工作的稳定帧不会推进该值。
        [[nodiscard]] std::uint64_t generation() const noexcept;

        /// 返回当前已提交 snapshot；首次 commit 前为空。
        [[nodiscard]] const world::RenderWorldSnapshotPtr& snapshot() const noexcept;

        /// 返回当前已提交活动 GPU 布局。
        [[nodiscard]] const GpuSceneLayout& layout() const noexcept;

        /**
         * 清空提交状态和稳定索引注册表。
         *
         * 调用方必须先等待设备空闲并销毁或失效全部 GPU Scene 资源；下一次规划将从索引 0 完整初始化。
         */
        void clear() noexcept;

    private:
        std::uint64_t epoch_ = 1;
        std::uint64_t generation_ = 0;
        world::RenderWorldSnapshotPtr snapshot_;
        GpuSceneLayout layout_;
        std::vector<GpuMeshBinding> meshIndexRegistry_;
        std::vector<GpuInstanceBinding> instanceIndexRegistry_;
        std::uint32_t nextMeshIndex_ = 0;
        std::uint32_t nextInstanceIndex_ = 0;
    };

} // namespace lumin::render::gpu

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "assets/ObjLoader.hpp"
#include "scene/Level.hpp"

namespace lumin::render::world {

    /** 渲染世界内使用的紧凑网格索引。 */
    using RenderMeshIndex = std::uint32_t;

    /**
     * 场景到渲染世界的变化类别。
     *
     * 这些位可以组合；渲染后端可据此只重建受影响的 GPU 资源。
     */
    enum class SceneChangeMask : std::uint8_t {
        /** 场景的可渲染数据没有变化。 */
        None = 0,
        /** 被引用的网格集合或网格内容发生变化。 */
        Geometry = 1U << 0U,
        /** 模型实例增删、稳定句柄或实例到网格的映射发生变化。 */
        InstanceTopology = 1U << 1U,
        /** 已存在实例的变换或材质发生变化。 */
        TransformOrMaterial = 1U << 2U,
        /** 材质纹理绑定发生变化，需要重建 descriptor 或 bindless 表。 */
        MaterialBinding = 1U << 3U,
        /** 太阳光参数发生变化。 */
        Lighting = 1U << 4U,
        /** 大气物理参数发生变化。 */
        Atmosphere = 1U << 5U,
        /** 所有渲染世界数据均需要初始化。 */
        All = (1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 3U) | (1U << 4U) | (1U << 5U),
    };

    /** 组合两个场景变化位。 */
    [[nodiscard]] constexpr SceneChangeMask operator|(SceneChangeMask left, SceneChangeMask right) noexcept {
        return static_cast<SceneChangeMask>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /** 取得两个场景变化位的交集。 */
    [[nodiscard]] constexpr SceneChangeMask operator&(SceneChangeMask left, SceneChangeMask right) noexcept {
        return static_cast<SceneChangeMask>(static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
    }

    /** 将场景变化位合并到左操作数。 */
    constexpr SceneChangeMask& operator|=(SceneChangeMask& left, SceneChangeMask right) noexcept {
        left = left | right;
        return left;
    }

    /** 判断变化集合是否包含给定类别中的任意一项。 */
    [[nodiscard]] constexpr bool hasAnyChange(SceneChangeMask changes, SceneChangeMask categories) noexcept {
        return (changes & categories) != SceneChangeMask::None;
    }

    /** 渲染世界拥有的一份网格数据。 */
    struct RenderWorldMesh {
        /** 网格在源场景中的稳定句柄，仅用于身份比较。 */
        scene::MeshHandle sourceHandle;
        /** 与场景存储完全解耦的网格副本。 */
        assets::Mesh mesh;
    };

    /** 渲染世界拥有的一份模型实例数据。 */
    struct RenderWorldInstance {
        /** 模型在源场景中的稳定句柄。 */
        scene::ModelHandle modelHandle;
        /** 指向同一快照 `meshes()` 数组的紧凑索引。 */
        RenderMeshIndex meshIndex = 0;
        /** 模型实例副本；其中包含源网格句柄、变换和完整材质。 */
        scene::ModelInstance model;
    };

    /**
     * 渲染器拥有的不可变场景快照。
     *
     * 快照只保存值，不引用 `scene::Level` 的容器、指针或 span。实例和网格按稳定句柄排序，
     * 因而场景内部的稠密数组换序不会改变快照顺序。通过 `shared_ptr<const RenderWorldSnapshot>`
     * 持有旧快照时，后续同步不会修改或释放其中的数据。连续快照可以共享未变化的只读存储，
     * 但不会共享任何源场景存储。
     */
    class RenderWorldSnapshot {
    public:
        /** 返回提取时源场景的总修订号。 */
        [[nodiscard]] std::uint64_t sourceRevision() const noexcept;

        /** 返回提取时源场景的拓扑修订号。 */
        [[nodiscard]] std::uint64_t sourceTopologyRevision() const noexcept;

        /** 返回提取时源场景的模型数据修订号。 */
        [[nodiscard]] std::uint64_t sourceModelRevision() const noexcept;

        /** 返回提取时源场景的太阳光修订号。 */
        [[nodiscard]] std::uint64_t sourceLightingRevision() const noexcept;

        /** 返回提取时源场景的大气参数修订号。 */
        [[nodiscard]] std::uint64_t sourceAtmosphereRevision() const noexcept;

        /** 返回由快照拥有的场景环境副本。 */
        [[nodiscard]] const scene::SceneEnvironment& environment() const noexcept;

        /** 返回快照拥有的唯一网格；数组按源 `MeshHandle` 排序。 */
        [[nodiscard]] const std::vector<RenderWorldMesh>& meshes() const noexcept;

        /** 返回快照拥有的模型实例；数组按稳定 `ModelHandle` 排序。 */
        [[nodiscard]] const std::vector<RenderWorldInstance>& instances() const noexcept;

        /**
         * 按稳定句柄查找模型实例。
         *
         * @return 找到时返回只读实例指针，否则返回 `nullptr`。该指针只指向本快照拥有的数据。
         */
        [[nodiscard]] const RenderWorldInstance* findInstance(scene::ModelHandle handle) const noexcept;

    private:
        friend class RenderWorldCache;
        friend class RenderWorldExtractor;

        RenderWorldSnapshot(std::uint64_t sourceRevision, std::uint64_t sourceTopologyRevision,
                            std::uint64_t sourceModelRevision, std::uint64_t sourceLightingRevision,
                            std::uint64_t sourceAtmosphereRevision, scene::SceneEnvironment environment,
                            std::vector<RenderWorldMesh> meshes, std::vector<RenderWorldInstance> instances);
        RenderWorldSnapshot(std::uint64_t sourceRevision, std::uint64_t sourceTopologyRevision,
                            std::uint64_t sourceModelRevision, std::uint64_t sourceLightingRevision,
                            std::uint64_t sourceAtmosphereRevision, scene::SceneEnvironment environment,
                            std::shared_ptr<const std::vector<RenderWorldMesh>> meshes,
                            std::shared_ptr<const std::vector<RenderWorldInstance>> instances);

        std::uint64_t sourceRevision_ = 0;
        std::uint64_t sourceTopologyRevision_ = 0;
        std::uint64_t sourceModelRevision_ = 0;
        std::uint64_t sourceLightingRevision_ = 0;
        std::uint64_t sourceAtmosphereRevision_ = 0;
        scene::SceneEnvironment environment_{};
        std::shared_ptr<const std::vector<RenderWorldMesh>> meshes_;
        std::shared_ptr<const std::vector<RenderWorldInstance>> instances_;
    };

    /** 不可变渲染世界快照的共享所有权类型。 */
    using RenderWorldSnapshotPtr = std::shared_ptr<const RenderWorldSnapshot>;

    /**
     * @brief 比较两个不可变世界快照并返回内容变化。
     *
     * `previous` 为空时返回
     * `SceneChangeMask::All`，相同共享对象返回 `SceneChangeMask::None`。该函数用于
     * Runtime
     * 相对最近成功提交快照重新计算变化，避免 latest-wins 丢帧漏掉一次性修订。
     */
    [[nodiscard]] SceneChangeMask changesBetween(const RenderWorldSnapshotPtr& previous,
                                                 const RenderWorldSnapshotPtr& current);

    /** 一次渲染世界同步的结果。 */
    struct SceneDelta {
        /** 与上一次同步结果相比发生的变化。 */
        SceneChangeMask changes = SceneChangeMask::None;
        /** 同步后的当前不可变快照。 */
        RenderWorldSnapshotPtr snapshot;

        /** 判断本次同步是否包含给定变化类别中的任意一项。 */
        [[nodiscard]] bool has(SceneChangeMask categories) const noexcept;

        /** 判断本次同步是否改变了可渲染数据。 */
        [[nodiscard]] bool changed() const noexcept;
    };

    /** 从场景值对象构建独立渲染快照的提取器。 */
    class RenderWorldExtractor {
    public:
        /**
         * 提取当前场景。
         *
         * 仅复制至少被一个模型实例引用的网格；多个实例引用同一 `MeshHandle` 时只复制一次。
         * 调用期间不得并发修改 `level`。
         */
        [[nodiscard]] static RenderWorldSnapshotPtr extract(const scene::Level& level);
    };

    /** 保存最近快照并计算场景增量的缓存。 */
    class RenderWorldCache {
    public:
        /**
         * 将缓存同步到场景当前状态。
         *
         * 首次同步或切换到另一个 `Level` 时返回 `SceneChangeMask::All`。源修订号未改变时会复用同一个
         * 快照对象；其余情况会发布全新的不可变快照，并通过内容比较消除修订号造成的伪变化。
         * 仅变换或材质变化时，新旧快照共享 renderer-owned 的只读网格存储。
         */
        [[nodiscard]] SceneDelta sync(const scene::Level& level);

        /** 返回最近一次同步生成的快照；同步前返回空指针。 */
        [[nodiscard]] RenderWorldSnapshotPtr snapshot() const noexcept;

        /** 清空缓存；下一次同步将被视为首次同步。 */
        void clear() noexcept;

    private:
        const scene::Level* sourceLevel_ = nullptr;
        RenderWorldSnapshotPtr snapshot_;
    };

} // namespace lumin::render::world

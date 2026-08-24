#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "render/atmosphere/AtmosphereLutPassPlan.hpp"
#include "render/gpu/GpuSceneResources.hpp"
#include "render/resources/FrameGraph.hpp"

namespace lumin::render {
    class ShaderLibrary;
}

namespace lumin::render::gi {

    /** SHARC 1.6.5 每个 hash-grid 槽位的物理字节布局。 */
    struct SharcBufferStrides {
        static constexpr std::uint32_t hashEntry = 8;
        static constexpr std::uint32_t accumulation = 16;
        static constexpr std::uint32_t resolved = 16;
        static constexpr std::uint32_t lock = 4;
    };

    /** GPU statistics buffer 中四个 32-bit counter 的稳定索引。 */
    struct SharcStatisticsLayout {
        static constexpr std::uint32_t queryHit = 0;
        static constexpr std::uint32_t update = 1;
        static constexpr std::uint32_t overflow = 2;
        static constexpr std::uint32_t occupancy = 3;
        static constexpr std::uint32_t count = 4;
        static constexpr std::uint32_t byteSize = count * sizeof(std::uint32_t);
    };

    /** 最近一帧已完成 GPU readback 的 SHARC 运行统计。 */
    struct SharcStatistics {
        /// query 阶段成功提前终止的二次表面数量。
        std::uint32_t queryHitCount = 0;
        /// update 阶段成功写入或复用 cache entry 的表面数量。
        std::uint32_t updateCount = 0;
        /// 线性探测窗口耗尽、无法插入 hash entry 的数量。
        std::uint32_t overflowCount = 0;
        /// resolve/evict 完成后仍有效的 hash entry 数量。
        std::uint32_t occupancyCount = 0;

        friend constexpr bool operator==(const SharcStatistics&, const SharcStatistics&) noexcept = default;
    };

    /** SHARC cache 的容量、采样密度和时序响应策略。 */
    struct SharcRadianceCacheConfig {
        /// 四个主 buffer 的共同 entry 数；必须为不小于 16 的 2 次幂。
        std::uint32_t capacity = 1U << 20U;
        /// 每个 tile 每帧只更新一个像素；vendor 推荐值为 5。
        std::uint32_t sparseTileSize = 5;
        /// 稳态 resolve 最多累积的帧数。
        std::uint32_t accumulationFrameCount = 64;
        /// 灯光、大气或材质变化期间使用的短累积窗口。
        std::uint32_t responsiveAccumulationFrameCount = 4;
        /// 响应式短窗口持续的成功提交帧数。
        std::uint32_t responsiveFrameCount = 8;
        /// entry 无新样本后被 resolve 驱逐前允许的帧数。
        std::uint32_t staleFrameCount = 64;
        /// logarithmic hash grid 的场景尺度；越大得到越小的 voxel。
        float sceneScale = 50.0F;
        /// 相邻 grid level 的尺寸比，SHARC 默认值为 2。
        float logarithmBase = 2.0F;
        /// grid level 选择偏移。
        float levelBias = 0.0F;
        /// 32-bit 整数辐亮度累积的量化尺度。
        float radianceScale = 1000.0F;
        /// 启用 vendor anti-firefly 权重抑制。
        bool enableAntiFireflyFilter = true;
    };

    /** 校验所有会进入 vendor shader 的 SHARC 配置。 */
    [[nodiscard]] bool validateSharcRadianceCacheConfig(const SharcRadianceCacheConfig& config) noexcept;

    /** 当前场景变化对 SHARC 跨帧 cache 的影响。 */
    struct SharcInvalidationInputs {
        bool cameraCut = false;
        bool topologyChanged = false;
        bool geometryChanged = false;
        bool materialChanged = false;
        bool lightingChanged = false;
        bool atmosphereChanged = false;
    };

    /** 本帧使用的 cache 失效等级。 */
    enum class SharcInvalidationMode : std::uint8_t {
        Preserve,
        ResponsiveDecay,
        FullReset,
    };

    /** CPU 侧为一个尚未提交帧生成的事务计划。 */
    struct SharcHistoryPlan {
        SharcInvalidationMode invalidation = SharcInvalidationMode::FullReset;
        std::uint32_t accumulationFrameCount = 1;
        std::uint32_t responsiveFramesAfterSubmit = 0;
        std::uint32_t resolveFrameIndex = 0;
        glm::vec4 previousCameraPosition{0.0F, 0.0F, 0.0F, 1.0F};

        /** 仅由 `SharcHistoryTracker::beginFrame()` 创建的计划才有效。 */
        [[nodiscard]] bool isValid() const noexcept;

    private:
        friend class SharcHistoryTracker;
        bool valid_ = false;
    };

    /** SHARC cache 的成功提交历史；`discard` 不改变任何已发布状态。 */
    class SharcHistoryTracker final {
    public:
        explicit SharcHistoryTracker(SharcRadianceCacheConfig config);

        /** 创建候选历史计划；存在尚未提交计划时抛出 `std::logic_error`。 */
        [[nodiscard]] SharcHistoryPlan beginFrame(const glm::vec4& cameraPosition,
                                                  const SharcInvalidationInputs& invalidation);

        /** 仅在 GPU submit 成功后发布候选历史。 */
        void commitSubmittedFrame();

        /** 放弃候选历史，下一次 `beginFrame()` 会从同一已提交状态重试。 */
        void discardPendingFrame() noexcept;

        [[nodiscard]] bool initialized() const noexcept;
        [[nodiscard]] bool hasPendingFrame() const noexcept;
        [[nodiscard]] std::uint64_t submittedFrameCount() const noexcept;
        [[nodiscard]] std::uint32_t responsiveFramesRemaining() const noexcept;
        [[nodiscard]] const glm::vec4& previousCameraPosition() const noexcept;

    private:
        struct Pending {
            SharcHistoryPlan plan;
            glm::vec4 currentCameraPosition{0.0F, 0.0F, 0.0F, 1.0F};
        };

        SharcRadianceCacheConfig config_;
        bool initialized_ = false;
        std::uint64_t submittedFrameCount_ = 0;
        std::uint32_t responsiveFramesRemaining_ = 0;
        glm::vec4 previousCameraPosition_{0.0F, 0.0F, 0.0F, 1.0F};
        std::optional<Pending> pending_;
    };

    /** 与 `shaders/include/SharcRuntime.slang` 精确同构的逐帧常量。 */
    struct alignas(16) SharcGpuConstants {
        /// xyz=current camera world position，w=sceneScale。
        glm::vec4 cameraPositionSceneScale{0.0F, 0.0F, 0.0F, 50.0F};
        /// xyz=previous submitted camera position，w=logarithmBase。
        glm::vec4 previousCameraPositionLogarithmBase{0.0F, 0.0F, 0.0F, 2.0F};
        /// xyz=从表面指向太阳的单位向量，w=radianceScale。
        glm::vec4 toSunWorldRadianceScale{0.0F, 1.0F, 0.0F, 1000.0F};
        /// rgb=预曝光太阳入射照度，w=天空亮度尺度。
        glm::vec4 sunIrradiance{1.0F};
        /// x=minT，y=maxT，z=levelBias，w=anti-firefly 开关。
        glm::vec4 traceParameters{0.001F, 10000.0F, 0.0F, 1.0F};
        /// x=capacity，y=accumulation frames，z=stale frames，w=成功提交序号。
        glm::uvec4 cacheParameters{0U};
        /// x=render width，y=render height，z=sparse tile size，w=保留。
        glm::uvec4 renderParameters{0U};
        glm::uvec4 reserved{0U};
    };

    static_assert(std::is_standard_layout_v<SharcGpuConstants>);
    static_assert(sizeof(SharcGpuConstants) == 128);
    static_assert(alignof(SharcGpuConstants) == 16);
    static_assert(offsetof(SharcGpuConstants, cameraPositionSceneScale) == 0);
    static_assert(offsetof(SharcGpuConstants, previousCameraPositionLogarithmBase) == 16);
    static_assert(offsetof(SharcGpuConstants, toSunWorldRadianceScale) == 32);
    static_assert(offsetof(SharcGpuConstants, sunIrradiance) == 48);
    static_assert(offsetof(SharcGpuConstants, traceParameters) == 64);
    static_assert(offsetof(SharcGpuConstants, cacheParameters) == 80);
    static_assert(offsetof(SharcGpuConstants, renderParameters) == 96);
    static_assert(offsetof(SharcGpuConstants, reserved) == 112);

    /** 构建常量时由宿主提供的当前帧参数。 */
    struct SharcFrameParameters {
        glm::vec4 cameraPosition{0.0F, 0.0F, 0.0F, 1.0F};
        glm::vec4 toSunWorld{0.0F, 1.0F, 0.0F, 0.0F};
        /// 供 SHARC 路径计算 Cook-Torrance 直接光的预曝光太阳入射照度。
        glm::vec4 sunIrradiance{1.0F};
        std::uint32_t renderWidth = 0;
        std::uint32_t renderHeight = 0;
        float minTraceDistance = 0.001F;
        float maxTraceDistance = 10000.0F;
    };

    /** 从已验证配置、候选历史和当前帧输入生成 shader ABI。 */
    [[nodiscard]] SharcGpuConstants buildSharcGpuConstants(const SharcRadianceCacheConfig& config,
                                                           const SharcHistoryPlan& history,
                                                           const SharcFrameParameters& frame);

    /** 持久 cache buffer 与当前帧槽常量的原生 NvRHI handles。 */
    struct SharcNativeResources {
        nvrhi::BufferHandle hashEntries;
        nvrhi::BufferHandle accumulation;
        nvrhi::BufferHandle resolved;
        nvrhi::BufferHandle lock;
        nvrhi::BufferHandle statistics;
        nvrhi::BufferHandle constants;

        [[nodiscard]] bool isValid() const noexcept {
            return hashEntries && accumulation && resolved && lock && statistics && constants;
        }
    };

    /** 一次 frame 中所有 SHARC buffer 的唯一 FrameGraph 身份。 */
    struct SharcFrameGraphResources {
        FrameGraphResourceHandle hashEntries;
        FrameGraphResourceHandle accumulation;
        FrameGraphResourceHandle resolved;
        FrameGraphResourceHandle lock;
        FrameGraphResourceHandle statistics;
        FrameGraphResourceHandle constants;
        FrameGraphResourceHandle statisticsReadback;

        [[nodiscard]] bool isValid() const noexcept {
            return hashEntries.isValid() && accumulation.isValid() && resolved.isValid() && lock.isValid() &&
                   statistics.isValid() && constants.isValid() && statisticsReadback.isValid();
        }
    };

    /** RT GI 与 SHARC update 共用的 descriptor set 2 大气绑定。 */
    struct RayTracingEnvironmentBindings {
        nvrhi::BindingSetHandle atmosphere;

        [[nodiscard]] bool isValid() const noexcept {
            return static_cast<bool>(atmosphere);
        }
    };

    /** 四张大气 LUT 与帧槽常量在当前 FrameGraph 中的唯一资源身份。 */
    struct RayTracingEnvironmentGraphResources {
        atmosphere::AtmosphereLutFrameGraphResources atmosphere;

        [[nodiscard]] bool isValid() const noexcept {
            return atmosphere.isValid();
        }
    };

    /** sparse update raygen 读取的 RT surface 物理纹理。 */
    struct SharcUpdateFrameInputs {
        nvrhi::TextureHandle position;
        nvrhi::TextureHandle normalRoughness;
        nvrhi::TextureHandle albedoMetallic;
        /// 用于恢复主命中点材质并计算 Cook-Torrance 漫反射吞吐量。
        nvrhi::TextureHandle materialId;
    };

    /** sparse update 对应的 FrameGraph 纹理身份。 */
    struct SharcUpdateFrameGraphInputs {
        FrameGraphResourceHandle position;
        FrameGraphResourceHandle normalRoughness;
        FrameGraphResourceHandle albedoMetallic;
        /// 与 `SharcUpdateFrameInputs::materialId` 对应的图资源身份。
        FrameGraphResourceHandle materialId;
    };

    /** sparse RT update 使用的当前 GPU Scene 物理 descriptor。 */
    struct SharcUpdateSceneBindings {
        gpu::GpuSceneDescriptors descriptors;
        std::span<const gpu::GpuGeometryDescriptor> geometry;
        std::span<const nvrhi::TextureHandle> baseColorTextures;
        std::span<const nvrhi::TextureHandle> normalRoughnessTextures;
        nvrhi::SamplerHandle materialSampler;
    };

    /** 必须复用 GPU Scene build 阶段导入的 FrameGraph 资源身份。 */
    struct SharcUpdateSceneGraphResources {
        FrameGraphResourceHandle tlas;
        FrameGraphResourceHandle instances;
        FrameGraphResourceHandle materials;
        std::span<const FrameGraphResourceHandle> vertices;
        std::span<const FrameGraphResourceHandle> indices;
        std::span<const FrameGraphResourceHandle> baseColorTextures;
        std::span<const FrameGraphResourceHandle> normalRoughnessTextures;
        FrameGraphPassHandle readyPass;
    };

    /** cache owner 注册 clear、sparse update 和 resolve 后返回的 query 输入。 */
    struct SharcGraphRecord {
        SharcNativeResources native;
        SharcFrameGraphResources resources;
        FrameGraphPassHandle clearPass;
        FrameGraphPassHandle updatePass;
        FrameGraphPassHandle resolvePass;
        SharcInvalidationMode invalidation = SharcInvalidationMode::FullReset;

        [[nodiscard]] bool isValid() const noexcept {
            return native.isValid() && resources.isValid() && clearPass.isValid() && updatePass.isValid() &&
                   resolvePass.isValid();
        }
    };

    /** 创建 SHARC cache pipeline 与持久资源所需参数。 */
    struct SharcRadianceCacheCreateInfo {
        /** NvRHI 设备；生命周期必须覆盖 cache。 */
        nvrhi::IDevice* device = nullptr;
        /** Session 级 shader 缓存；生命周期必须覆盖创建过程。 */
        ShaderLibrary* shaders = nullptr;
        std::uint32_t frameSlotCount = 0;
        std::uint32_t maxGeometryDescriptors = 0;
        std::uint32_t maxMaterialTextureDescriptors = 0;
        /// 与 raster sky 完全相同的 descriptor set 2 layout。
        nvrhi::BindingLayoutHandle atmosphereBindingLayout;
        SharcRadianceCacheConfig config;
        std::span<const SharcUpdateFrameInputs> frames;
    };

    namespace detail {

        enum class SharcBufferKind : std::uint8_t {
            HashEntries,
            Accumulation,
            Resolved,
            Lock,
            Statistics,
            StatisticsReadback,
            Constants,
        };

        /** 为指定用途构造精确 stride、CPU access 与状态跟踪描述。 */
        [[nodiscard]] nvrhi::BufferDesc makeSharcBufferDesc(SharcBufferKind kind, std::uint32_t capacity,
                                                            const char* debugName);

        /** 构造 sparse update RT pipeline 的 descriptor set 0 layout。 */
        [[nodiscard]] nvrhi::BindingLayoutDesc
        makeSharcUpdateBindingLayoutDesc(std::uint32_t maxGeometryDescriptors,
                                         std::uint32_t maxMaterialTextureDescriptors = 1);

        /** 构造 resolve compute pipeline 的 descriptor set 0 layout。 */
        [[nodiscard]] nvrhi::BindingLayoutDesc makeSharcResolveBindingLayoutDesc();

        /** sparse update 每个 dispatch thread 对应一个 tile。 */
        struct SharcUpdateDispatchSize {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
        };

        [[nodiscard]] SharcUpdateDispatchSize makeSharcUpdateDispatchSize(std::uint32_t width, std::uint32_t height,
                                                                          std::uint32_t tileSize);

        /** resolve compute 使用 256 threads/group 覆盖全部 cache entries。 */
        [[nodiscard]] std::uint32_t makeSharcResolveDispatchSize(std::uint32_t capacity);

        template <typename CommandList>
        void recordSharcUpdateDispatch(CommandList& commandList, const nvrhi::rt::State& state,
                                       SharcUpdateDispatchSize dispatch) {
            commandList.setRayTracingState(state);
            commandList.dispatchRays(nvrhi::rt::DispatchRaysArguments().setDimensions(dispatch.width, dispatch.height));
        }

        template <typename CommandList>
        void recordSharcResolveDispatch(CommandList& commandList, const nvrhi::ComputeState& state,
                                        std::uint32_t groupsX) {
            commandList.setComputeState(state);
            commandList.dispatch(groupsX, 1, 1);
        }

    } // namespace detail

    /**
     * SHARC 1.6.5 cache 的物理资源、sparse RT update、resolve/evict 与统计 readback owner。
     *
     * 实现固定使用 `SHARC_ENABLE_64_BIT_ATOMICS=0`，因此始终创建 vendor lock buffer。`record()` 只注册
     * FrameGraph 访问和命令，调用方必须在 submit 成功后调用 `commitSubmittedFrame()`；录制或提交失败则调用
     * `discardPendingFrame()`，cache 历史不会前进。
     */
    class SharcRadianceCache final {
    public:
        explicit SharcRadianceCache(const SharcRadianceCacheCreateInfo& createInfo);
        ~SharcRadianceCache();

        SharcRadianceCache(const SharcRadianceCache&) = delete;
        SharcRadianceCache& operator=(const SharcRadianceCache&) = delete;

        /** 注册 clear -> sparse update -> resolve/evict，并返回同一组 query 资源身份。 */
        [[nodiscard]] SharcGraphRecord record(FrameGraph& frameGraph, std::uint32_t frameSlot,
                                              bool frameSlotFenceWaited, const SharcFrameParameters& frameParameters,
                                              const SharcInvalidationInputs& invalidation,
                                              const RayTracingEnvironmentBindings& environment,
                                              const RayTracingEnvironmentGraphResources& environmentResources,
                                              const SharcUpdateFrameGraphInputs& inputs,
                                              const SharcUpdateSceneBindings& scene,
                                              const SharcUpdateSceneGraphResources& sceneResources);

        /**
         * 在 RT query pass 之后复制四个 counter 到当前帧槽 readback buffer。
         * 返回的 transfer pass 也由 FrameGraph 负责全部状态转换。
         */
        [[nodiscard]] FrameGraphPassHandle recordStatisticsReadback(FrameGraph& frameGraph,
                                                                    const SharcGraphRecord& record,
                                                                    FrameGraphPassHandle queryPass);

        /** 成功提交本帧 cache 工作后发布 history/readback 状态。 */
        void commitSubmittedFrame();

        /** 放弃候选 history/readback 状态。 */
        void discardPendingFrame() noexcept;

        [[nodiscard]] bool hasPendingFrame() const noexcept;

        /**
         * 对应帧槽 fence 已完成后读取上一次提交的统计；该槽位尚无统计时返回 `std::nullopt`。
         */
        [[nodiscard]] std::optional<SharcStatistics> readbackStatistics(std::uint32_t frameSlot,
                                                                        bool frameSlotFenceWaited);

        [[nodiscard]] const SharcRadianceCacheConfig& config() const noexcept;
        [[nodiscard]] const SharcHistoryTracker& history() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::gi

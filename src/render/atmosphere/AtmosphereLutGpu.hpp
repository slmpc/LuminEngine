#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <nvrhi/nvrhi.h>

#include "render/atmosphere/AtmosphereGpuConstants.hpp"
#include "render/atmosphere/AtmosphereLutPassPlan.hpp"

namespace lumin::render::atmosphere {

    /** 单个大气 LUT compute 入口的线程组尺寸。 */
    struct AtmosphereLutThreadGroupSize {
        std::uint32_t x = 1;
        std::uint32_t y = 1;
        std::uint32_t z = 1;

        friend constexpr bool operator==(const AtmosphereLutThreadGroupSize&,
                                         const AtmosphereLutThreadGroupSize&) noexcept = default;
    };

    /** 一次 `dispatch` 使用的线程组数量。 */
    struct AtmosphereLutDispatchSize {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t z = 0;

        /** 返回三个维度是否都可提交。 */
        [[nodiscard]] constexpr bool isValid() const noexcept {
            return x != 0 && y != 0 && z != 0;
        }

        friend constexpr bool operator==(const AtmosphereLutDispatchSize&,
                                         const AtmosphereLutDispatchSize&) noexcept = default;
    };

    /** GPU owner 持有并供 raster、ray tracing 与 GI 共享的物理 LUT。 */
    struct AtmosphereLutNativeResources {
        std::array<nvrhi::TextureHandle, atmosphereLutResourceCount> textures{};
        nvrhi::SamplerHandle sampler;

        /** 返回指定 LUT 的 NvRHI 纹理。 */
        [[nodiscard]] const nvrhi::TextureHandle& texture(AtmosphereLut lut) const;

        /** 返回四张纹理和 sampler 是否均已创建。 */
        [[nodiscard]] bool isValid() const noexcept;
    };

    /** 创建持久化大气 LUT 资源和 compute pipelines 所需的参数。 */
    struct AtmosphereLutGpuCreateInfo {
        /// NvRHI 设备；生命周期必须覆盖 GPU owner。
        nvrhi::IDevice* device = nullptr;
        /// CMake 生成的 SPIR-V 根目录。
        std::filesystem::path shaderDirectory;
        /// 可同时在途的帧槽数量；每槽拥有独立常量缓冲。
        std::uint32_t frameSlotCount = 0;
        /// LUT 尺寸与格式质量档。
        AtmosphereLutQuality quality;
    };

    /** `record()` 返回的同一组 FrameGraph 身份和实际 compute pass 句柄。 */
    struct AtmosphereLutGraphRecord {
        AtmosphereLutFrameGraphResources resources;
        AtmosphereLutFrameGraphPasses passes;
    };

    namespace detail {

        /** 将后端无关资源契约转换为 NvRHI 纹理描述。 */
        [[nodiscard]] nvrhi::TextureDesc makeAtmosphereLutTextureDesc(const AtmosphereLutResourceDesc& resource);

        /** 构造逐帧槽大气常量缓冲描述。 */
        [[nodiscard]] nvrhi::BufferDesc makeAtmosphereLutConstantBufferDesc();

        /** 构造 LUT 共用的 linear-clamp sampler。 */
        [[nodiscard]] nvrhi::SamplerDesc makeAtmosphereLutSamplerDesc() noexcept;

        /** 构造与指定 Slang compute 入口精确同构的 descriptor set 0 layout。 */
        [[nodiscard]] nvrhi::BindingLayoutDesc makeAtmosphereLutBindingLayoutDesc(AtmosphereLut target);

        /** 构造指定帧槽、指定目标 pass 的不可变 binding set。 */
        [[nodiscard]] nvrhi::BindingSetDesc
        makeAtmosphereLutBindingSetDesc(AtmosphereLut target, nvrhi::IBuffer* constants,
                                        const AtmosphereLutNativeResources& resources);

        /** 构造 raster sky 与 RT miss 共用的 descriptor set 2 layout。 */
        [[nodiscard]] nvrhi::BindingLayoutDesc makeAtmosphereConsumerBindingLayoutDesc();

        /** 将同一组 LUT 与帧槽常量写入共享 consumer binding set。 */
        [[nodiscard]] nvrhi::BindingSetDesc
        makeAtmosphereConsumerBindingSetDesc(nvrhi::IBuffer* constants, const AtmosphereLutNativeResources& resources);

        /** 返回 Slang 入口声明的线程组尺寸。 */
        [[nodiscard]] AtmosphereLutThreadGroupSize atmosphereLutThreadGroupSize(AtmosphereLut target);

        /** 对任意合法质量尺寸计算向上取整后的 dispatch 大小。 */
        [[nodiscard]] AtmosphereLutDispatchSize
        makeAtmosphereLutDispatchSize(const AtmosphereLutResourceDesc& resource);

        /** 先绑定 compute state，再提交精确覆盖目标 LUT 的线程组。 */
        template <typename CommandList>
        void recordAtmosphereLutDispatch(CommandList& commandList, const nvrhi::ComputeState& state,
                                         AtmosphereLutDispatchSize dispatch) {
            if (!dispatch.isValid()) {
                throw std::invalid_argument("Atmosphere LUT dispatch dimensions must be non-zero.");
            }
            commandList.setComputeState(state);
            commandList.dispatch(dispatch.x, dispatch.y, dispatch.z);
        }

    } // namespace detail

    /**
     * 大气 LUT 的 GPU 资源、pipeline、binding set 与 FrameGraph dispatch owner。
     *
     * 四张 LUT 跨帧持久化，常量缓冲按帧槽分配。`record()` 只能在对应槽位 fence 已等待后更新常量，且只声明
     * FrameGraph 资源访问，不录制手写 barrier。调用方必须以 `commitSubmittedFrame()` 或
     * `discardPendingFrame()` 结束每次成功的 `record()`；只有前者会推进跨帧初始状态。
     */
    class AtmosphereLutGpu final {
    public:
        explicit AtmosphereLutGpu(const AtmosphereLutGpuCreateInfo& createInfo);
        ~AtmosphereLutGpu();

        AtmosphereLutGpu(const AtmosphereLutGpu&) = delete;
        AtmosphereLutGpu& operator=(const AtmosphereLutGpu&) = delete;

        /**
         * 更新当前帧槽常量、导入持久资源并注册计划中的最小 compute pass 集。即使计划为空，也会更新常量，
         * 因为 raster、RT 与 GI consumer 会继续读取该帧槽。
         *
         * 首次成功提交前，计划必须重建全部四张 LUT。返回的资源身份必须由后续 raster/RT consumer 复用。
         *
         * @throws std::invalid_argument 参数或计划无效时抛出。
         * @throws std::logic_error 帧槽 fence 未等待、已有待提交记录或首次计划不完整时抛出。
         */
        [[nodiscard]] AtmosphereLutGraphRecord record(FrameGraph& frameGraph, std::uint32_t frameSlot,
                                                      bool frameSlotFenceWaited,
                                                      const AtmosphereGpuConstants& constants,
                                                      const AtmosphereLutPassPlan& plan);

        /** 成功提交后确认本次资源状态；无待提交记录时不执行操作。 */
        void commitSubmittedFrame() noexcept;

        /** 放弃录制或提交失败的帧，不推进任何资源状态。 */
        void discardPendingFrame() noexcept;

        /** 返回是否存在尚未确认提交的 `record()`。 */
        [[nodiscard]] bool hasPendingFrame() const noexcept;

        /** 返回四张物理 LUT 与共享 sampler。 */
        [[nodiscard]] const AtmosphereLutNativeResources& nativeResources() const noexcept;

        /** 返回指定帧槽的物理常量缓冲；仅能在该槽 fence 已等待后更新。 */
        [[nodiscard]] const nvrhi::BufferHandle& constantBuffer(std::uint32_t frameSlot) const;

        /** 返回创建时固定的质量配置。 */
        [[nodiscard]] const AtmosphereLutQuality& quality() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render::atmosphere

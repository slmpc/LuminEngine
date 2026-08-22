#pragma once

#include "render/resources/VulkanResources.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    /// 内置 Raster 阴影 Feature 的级联数量，必须与 shader ABI 一致。
    inline constexpr std::uint32_t shadowCascadeCount = 4;
    /// 每张 CSM 级联纹理的固定边长。
    inline constexpr std::uint32_t shadowMapResolution = 2048;

    /** Raster surface 与 shadow Feature 在一个帧槽中拥有的物理资源。 */
    struct RasterFeatureFrameResources {
        /// 世界空间位置 G-buffer。
        GpuTexture position;
        /// 世界空间法线与粗糙度 G-buffer。
        GpuTexture normalRoughness;
        /// 线性基础颜色与金属度 G-buffer。
        GpuTexture albedo;
        /// 当前到上一成功帧的屏幕运动矢量。
        GpuTexture motion;
        /// 每像素稳定 GPU material 索引；无几何处为 `GpuMaterialIndex::invalidValue`。
        GpuTexture materialId;
        /// Raster surface 深度附件。
        GpuTexture depth;
        /// 四张独立的 sampled CSM 深度附件。
        std::array<GpuTexture, shadowCascadeCount> shadowCascades{};
    };

    /**
     * @brief 独占 Raster surface 与 CSM 的逐帧物理资源。
     *
     * 资源只能在对应 frame-slot fence 已完成或 GPU idle 的安全边界创建、销毁。该对象不拥有 pipeline、GI、PostFX
     * 或历史资源。
     */
    class RasterFeatureResources final {
    public:
        /**
         * @brief 绑定资源设备并创建空的帧槽集合。
         * @param device 生命周期必须覆盖本对象。
         * @param frameSlotCount 并行帧槽数，必须大于零。
         * @throws std::invalid_argument `frameSlotCount` 为零时抛出。
         */
        RasterFeatureResources(nvrhi::IDevice& device, std::uint32_t frameSlotCount);
        /// 释放仍由该 Feature 持有的全部资源。
        ~RasterFeatureResources();

        /// Raster 资源 owner 不可复制。
        RasterFeatureResources(const RasterFeatureResources&) = delete;
        /// Raster 资源 owner 不可复制赋值。
        RasterFeatureResources& operator=(const RasterFeatureResources&) = delete;

        /**
         * @brief 为全部帧槽事务式创建指定范围的 Raster 资源。
         * @throws std::invalid_argument 范围为空时抛出。
         * @throws std::runtime_error 设备不支持所需格式或资源创建失败时抛出。
         */
        void create(std::uint32_t width, std::uint32_t height);
        /// 释放全部 Raster 资源；可重复调用。
        void destroy() noexcept;

        /**
         * @brief 返回指定帧槽的 Raster 资源。
         * @throws std::out_of_range 帧槽索引越界时抛出。
         */
        [[nodiscard]] const RasterFeatureFrameResources& frame(std::uint32_t frameIndex) const;
        /// 返回帧槽数量。
        [[nodiscard]] std::uint32_t frameSlotCount() const noexcept;
        /// 返回 position G-buffer 格式。
        [[nodiscard]] nvrhi::Format positionFormat() const noexcept;
        /// 返回 normal/roughness G-buffer 格式。
        [[nodiscard]] nvrhi::Format normalFormat() const noexcept;
        /// 返回 albedo/metallic G-buffer 格式。
        [[nodiscard]] nvrhi::Format albedoFormat() const noexcept;
        /// 返回 motion G-buffer 格式。
        [[nodiscard]] nvrhi::Format motionFormat() const noexcept;
        /// 返回 material ID G-buffer 格式。
        [[nodiscard]] nvrhi::Format materialIdFormat() const noexcept;
        /// 返回 surface depth 格式。
        [[nodiscard]] nvrhi::Format depthFormat() const noexcept;
        /// 返回 CSM depth 格式。
        [[nodiscard]] nvrhi::Format shadowDepthFormat() const noexcept;

    private:
        [[nodiscard]] nvrhi::Format chooseFormat(std::span<const nvrhi::Format> candidates,
                                                 nvrhi::FormatSupport required) const;
        [[nodiscard]] GpuTexture createTexture(const nvrhi::TextureDesc& desc) const;
        void createImages(std::uint32_t width, std::uint32_t height);

        nvrhi::IDevice& device_;
        GpuResourceManager resources_;
        std::vector<RasterFeatureFrameResources> frames_;
        nvrhi::Format positionFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format normalFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format albedoFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format motionFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format materialIdFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format depthFormat_ = nvrhi::Format::UNKNOWN;
        nvrhi::Format shadowDepthFormat_ = nvrhi::Format::UNKNOWN;
    };

} // namespace lumin::render

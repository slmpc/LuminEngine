#pragma once

#include "render/RenderSettings.hpp"
#include "render/core/RenderFeatureRegistry.hpp"
#include "render/core/RenderSettingsStore.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace lumin::render::pipelines {

    /** 内置 recipe 的稳定类型。 */
    enum class DefaultRenderPipelineKind : std::uint8_t {
        /// CSM、G-buffer 与屏幕空间 AO 路径。
        Raster,
        /// Primary RT surface、SHARC 与 NRD 路径。
        Hybrid,
    };

    /** 保存一条内置 recipe 及其全部静态 Feature 描述符。 */
    class DefaultRenderPipelineDefinition final {
    public:
        /** 使用给定 recipe 和描述符构造定义。 */
        DefaultRenderPipelineDefinition(core::RenderPipelineRecipe recipe,
                                        std::vector<core::FeatureDescriptor> descriptors);

        /// 返回不可变 recipe。
        [[nodiscard]] const core::RenderPipelineRecipe& recipe() const noexcept;

        /// 返回当前 recipe 需要注册的全部 Feature 描述符。
        [[nodiscard]] std::span<const core::FeatureDescriptor> descriptors() const noexcept;

        /**
         * @brief 按稳定标识返回描述符。
         * @throws std::out_of_range 标识不属于当前 recipe 时抛出。
         */
        [[nodiscard]] const core::FeatureDescriptor& descriptor(const core::FeatureId& id) const;

    private:
        core::RenderPipelineRecipe recipe_;
        std::vector<core::FeatureDescriptor> descriptors_;
    };

    /** 内置 Feature 的稳定标识。 */
    namespace feature_ids {

        [[nodiscard]] const core::FeatureId& shadow();
        [[nodiscard]] const core::FeatureId& rasterSurface();
        [[nodiscard]] const core::FeatureId& hybridSurface();
        [[nodiscard]] const core::FeatureId& atmosphere();
        [[nodiscard]] const core::FeatureId& globalIllumination();
        [[nodiscard]] const core::FeatureId& denoising();
        [[nodiscard]] const core::FeatureId& lightingComposite();
        [[nodiscard]] const core::FeatureId& temporalAa();
        [[nodiscard]] const core::FeatureId& toneMapping();
        [[nodiscard]] const core::FeatureId& presentation();

    } // namespace feature_ids

    /**
     * @brief 创建 Raster 或 Hybrid 内置 recipe。
     *
     * 数据依赖是排序的主要来源；显式 `after` 只用于 Presentation 等具有外部副作用的边界。
     */
    [[nodiscard]] DefaultRenderPipelineDefinition makeDefaultRenderPipeline(DefaultRenderPipelineKind kind);

    /** 向 registry 注册内置 Feature 的类型化设置、默认值、校验器和变化影响。 */
    void registerDefaultRenderSettings(core::RenderSettingsSchemaRegistry& registry);

    /**
     * @brief 将兼容层聚合设置转换为不可变 typed snapshot。
     * @throws std::invalid_argument 任一设置未通过对应 Feature 校验器时抛出。
     */
    [[nodiscard]] core::RenderSettingsSnapshot makeDefaultRenderSettingsSnapshot(const RenderSettings& settings);

} // namespace lumin::render::pipelines

#pragma once

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <array>
#include <span>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    namespace detail {

        /**
         * @brief 使用调用方提供的 shader 加载器和创建器构造全屏 graphics pipeline 描述。
         * @param vertexShader 顶点入口的类型化 ID。
         * @param fragmentShader 片元入口的类型化 ID。
         *
         * @param colorFormat 唯一颜色附件的格式。
         * @param bindingLayouts pipeline 使用的 descriptor layout 列表。
         * @param loadModule 可调用的 shader 加载器。
         * @param createPipeline 可调用的 pipeline 创建器。
         */
        template <typename LoadModule, typename CreatePipeline>
        void createFullscreenPipeline(ShaderId vertexShader, ShaderId fragmentShader, nvrhi::Format colorFormat,
                                      std::span<const nvrhi::BindingLayoutHandle> bindingLayouts,
                                      LoadModule& loadModule, CreatePipeline& createPipeline) {
            GraphicsPipelineDesc desc;
            desc.vertexShader = loadModule(vertexShader);
            desc.fragmentShader = loadModule(fragmentShader);
            const std::array<nvrhi::Format, 1> colors = {colorFormat};
            desc.bindingLayouts = bindingLayouts;
            desc.colorFormats = colors;
            desc.depthTestEnable = false;
            desc.depthWriteEnable = false;
            desc.cullMode = nvrhi::RasterCullMode::None;
            createPipeline(desc);
        }

    } // namespace detail

    /**
     * @brief 创建无顶点输入的全屏 graphics pipeline。
     *
     * Factory 只拥有通用 shader/pipeline 创建服务，不缓存或命名任何 Feature pipeline。返回的 handle 由调用方 Feature
     * 负责保存，并在其生命周期结束时释放。所有方法只能在拥有 NvRHI device 的渲染主线程调用。

     */
    class FullscreenPipelineFactory final {
    public:
        /**
         * @brief 绑定设备和 session 级 shader 缓存。
         * @param device 生命周期必须覆盖 Factory 及其创建的
         * pipeline。
         * @param shaders 生命周期必须覆盖 Factory。
         */
        FullscreenPipelineFactory(nvrhi::IDevice& device, ShaderLibrary& shaders);

        /**
         * @brief 创建一个全屏 graphics pipeline 并把所有权交给返回 handle。
         * @param vertexShader 顶点入口的类型化 ID。
         * @param fragmentShader 片元入口的类型化 ID。
         *
         * @param colorFormat 唯一颜色附件格式。
         * @param bindingLayouts pipeline descriptor layouts；只在调用期间读取。
         * @return 新创建的 NvRHI pipeline handle。
         * @throws std::runtime_error shader 加载或 pipeline 创建失败时抛出。
         */
        [[nodiscard]] nvrhi::GraphicsPipelineHandle
        create(ShaderId vertexShader, ShaderId fragmentShader, nvrhi::Format colorFormat,
               std::span<const nvrhi::BindingLayoutHandle> bindingLayouts) const;

    private:
        ShaderLibrary& shaders_;
        PipelineFactory factory_;
    };

} // namespace lumin::render

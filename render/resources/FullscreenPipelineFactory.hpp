#pragma once

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <array>
#include <filesystem>
#include <span>
#include <string>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    namespace detail {

        /**
         * @brief 使用调用方提供的 shader 加载器和创建器构造全屏 graphics pipeline 描述。
         * @param shaderName 不含 stage 后缀的 shader 模块名。
         * @param colorFormat 唯一颜色附件的格式。
         * @param bindingLayouts pipeline 使用的 descriptor layout 列表。
         * @param loadModule 可调用的 shader 加载器。
         * @param createPipeline 可调用的 pipeline 创建器。
         */
        template <typename LoadModule, typename CreatePipeline>
        void createFullscreenPipeline(const std::string& shaderName, nvrhi::Format colorFormat,
                                      std::span<const nvrhi::BindingLayoutHandle> bindingLayouts,
                                      LoadModule& loadModule, CreatePipeline& createPipeline) {
            GraphicsPipelineDesc desc;
            desc.vertexShader = loadModule(shaderName + ".vert.spv", nvrhi::ShaderType::Vertex);
            desc.fragmentShader = loadModule(shaderName + ".frag.spv", nvrhi::ShaderType::Pixel);
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
     * 负责保存，并在其生命周期结束时释放。所有方法只能在拥有 NvRHI device 的渲染线程调用。
     */
    class FullscreenPipelineFactory final {
    public:
        /**
         * @brief 绑定设备和 shader 输出目录。
         * @param device 生命周期必须覆盖 Factory 及其创建的 pipeline。
         * @param shaderDirectory 编译后 SPIR-V 所在目录。
         */
        FullscreenPipelineFactory(nvrhi::IDevice& device, std::filesystem::path shaderDirectory);

        /**
         * @brief 创建一个全屏 graphics pipeline 并把所有权交给返回 handle。
         * @param shaderName 不含 `.vert.spv`/`.frag.spv` 后缀的模块名。
         * @param colorFormat 唯一颜色附件格式。
         * @param bindingLayouts pipeline descriptor layouts；只在调用期间读取。
         * @return 新创建的 NvRHI pipeline handle。
         * @throws std::runtime_error shader 加载或 pipeline 创建失败时抛出。
         */
        [[nodiscard]] nvrhi::GraphicsPipelineHandle
        create(std::string shaderName, nvrhi::Format colorFormat,
               std::span<const nvrhi::BindingLayoutHandle> bindingLayouts) const;

    private:
        ShaderLibrary shaders_;
        PipelineFactory factory_;
    };

} // namespace lumin::render

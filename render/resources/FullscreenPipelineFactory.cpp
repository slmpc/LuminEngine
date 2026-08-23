#include "render/resources/FullscreenPipelineFactory.hpp"

namespace lumin::render {

    FullscreenPipelineFactory::FullscreenPipelineFactory(nvrhi::IDevice& device, ShaderLibrary& shaders)
        : shaders_(shaders), factory_(device) {
    }

    nvrhi::GraphicsPipelineHandle
    FullscreenPipelineFactory::create(ShaderId vertexShader, ShaderId fragmentShader, nvrhi::Format colorFormat,
                                      std::span<const nvrhi::BindingLayoutHandle> bindingLayouts) const {
        auto loadModule = [this](ShaderId id) {
            return shaders_.load(id);
        };
        nvrhi::GraphicsPipelineHandle result;
        auto createPipeline = [this, &result](const GraphicsPipelineDesc& desc) {
            result = factory_.createGraphicsPipeline(desc);
        };
        detail::createFullscreenPipeline(vertexShader, fragmentShader, colorFormat, bindingLayouts, loadModule,
                                         createPipeline);
        return result;
    }

} // namespace lumin::render

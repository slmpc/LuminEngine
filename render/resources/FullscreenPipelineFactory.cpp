#include "render/resources/FullscreenPipelineFactory.hpp"

#include <string_view>
#include <utility>

namespace lumin::render {

    FullscreenPipelineFactory::FullscreenPipelineFactory(nvrhi::IDevice& device, std::filesystem::path shaderDirectory)
        : shaders_(device, std::move(shaderDirectory)), factory_(device) {
    }

    nvrhi::GraphicsPipelineHandle
    FullscreenPipelineFactory::create(std::string shaderName, nvrhi::Format colorFormat,
                                      std::span<const nvrhi::BindingLayoutHandle> bindingLayouts) const {
        auto loadModule = [this](const std::string& moduleName, nvrhi::ShaderType type) {
            const std::string_view entryPoint = type == nvrhi::ShaderType::Vertex ? "vertexMain" : "fragmentMain";
            return shaders_.loadModule(moduleName, type, entryPoint);
        };
        nvrhi::GraphicsPipelineHandle result;
        auto createPipeline = [this, &result](const GraphicsPipelineDesc& desc) {
            result = factory_.createGraphicsPipeline(desc);
        };
        detail::createFullscreenPipeline(shaderName, colorFormat, bindingLayouts, loadModule, createPipeline);
        return result;
    }

} // namespace lumin::render

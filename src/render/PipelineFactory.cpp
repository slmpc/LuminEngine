#include "lumin/render/PipelineFactory.hpp"

#include <stdexcept>

namespace lumin::render {

    PipelineFactory::PipelineFactory(nvrhi::IDevice& device) : device_(device) {
    }

    nvrhi::GraphicsPipelineHandle PipelineFactory::createGraphicsPipeline(const GraphicsPipelineDesc& desc) const {
        if (desc.bindingLayouts.size() > nvrhi::c_MaxBindingLayouts) {
            throw std::invalid_argument("Too many binding layouts for graphics pipeline.");
        }
        if (desc.colorFormats.size() > nvrhi::c_MaxRenderTargets) {
            throw std::invalid_argument("Too many color formats for graphics pipeline.");
        }

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setInputLayout(desc.inputLayout)
            .setVertexShader(desc.vertexShader)
            .setFragmentShader(desc.fragmentShader);
        for (const nvrhi::BindingLayoutHandle& bindingLayout : desc.bindingLayouts) {
            pipelineDesc.addBindingLayout(bindingLayout);
        }

        pipelineDesc.renderState.rasterState.setFillSolid()
            .setCullMode(desc.cullMode)
            .setFrontCounterClockwise(true)
            .setDepthBias(desc.depthBiasEnable ? desc.depthBias : 0)
            .setSlopeScaleDepthBias(desc.depthBiasEnable ? desc.slopeScaledDepthBias : 0.0f);
        pipelineDesc.renderState.depthStencilState.setDepthTestEnable(desc.depthTestEnable)
            .setDepthWriteEnable(desc.depthWriteEnable)
            .setDepthFunc(desc.depthCompareOp);

        nvrhi::FramebufferInfo framebufferInfo;
        for (nvrhi::Format colorFormat : desc.colorFormats) {
            framebufferInfo.addColorFormat(colorFormat);
        }
        framebufferInfo.setDepthFormat(desc.depthFormat).setSampleCount(desc.sampleCount);

        nvrhi::GraphicsPipelineHandle pipeline = device_.createGraphicsPipeline(pipelineDesc, framebufferInfo);
        if (!pipeline) {
            throw std::runtime_error("Failed to create graphics pipeline.");
        }
        return pipeline;
    }

} // namespace lumin::render

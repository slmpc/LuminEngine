#pragma once

#include <cstdint>
#include <span>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    struct GraphicsPipelineDesc {
        nvrhi::ShaderHandle vertexShader;
        nvrhi::ShaderHandle fragmentShader;
        nvrhi::InputLayoutHandle inputLayout;
        std::span<const nvrhi::BindingLayoutHandle> bindingLayouts;
        std::span<const nvrhi::Format> colorFormats;
        nvrhi::Format depthFormat = nvrhi::Format::UNKNOWN;
        bool depthTestEnable = true;
        bool depthWriteEnable = true;
        nvrhi::ComparisonFunc depthCompareOp = nvrhi::ComparisonFunc::Less;
        nvrhi::RasterCullMode cullMode = nvrhi::RasterCullMode::Back;
        bool depthBiasEnable = false;
        int depthBias = 0;
        float slopeScaledDepthBias = 0.0f;
        std::uint32_t sampleCount = 1;
    };

    class PipelineFactory {
    public:
        explicit PipelineFactory(nvrhi::IDevice& device);

        [[nodiscard]] nvrhi::GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) const;

    private:
        nvrhi::IDevice& device_;
    };

} // namespace lumin::render

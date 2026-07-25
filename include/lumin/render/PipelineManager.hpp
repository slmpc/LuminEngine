#pragma once

#include <array>
#include <filesystem>
#include <string>

#include <nvrhi/nvrhi.h>

#include "lumin/render/PipelineFactory.hpp"
#include "lumin/render/ShaderLibrary.hpp"

namespace lumin::render {

    class VulkanContext;

    namespace detail {

        template <typename LoadModule, typename CreatePipeline>
        void createFullscreenPipeline(const std::string& shaderName, nvrhi::Format colorFormat,
                                      const std::array<nvrhi::BindingLayoutHandle, 1>& bindingLayouts,
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

    class PipelineManager {
    public:
        PipelineManager(VulkanContext& context, std::filesystem::path shaderDirectory);
        ~PipelineManager();

        PipelineManager(const PipelineManager&) = delete;
        PipelineManager& operator=(const PipelineManager&) = delete;

        void create(nvrhi::BindingLayoutHandle bindingLayout, nvrhi::Format lightingFormat,
                    nvrhi::Format swapchainFormat);
        void destroy() noexcept;

        [[nodiscard]] const nvrhi::GraphicsPipelineHandle& sky() const noexcept;
        [[nodiscard]] const nvrhi::GraphicsPipelineHandle& deferredLighting() const noexcept;
        [[nodiscard]] const nvrhi::GraphicsPipelineHandle& taa() const noexcept;
        [[nodiscard]] const nvrhi::GraphicsPipelineHandle& tonemap() const noexcept;
        [[nodiscard]] const nvrhi::GraphicsPipelineHandle& postprocess() const noexcept;

    private:
        ShaderLibrary shaders_;
        PipelineFactory factory_;
        nvrhi::GraphicsPipelineHandle sky_;
        nvrhi::GraphicsPipelineHandle deferredLighting_;
        nvrhi::GraphicsPipelineHandle taa_;
        nvrhi::GraphicsPipelineHandle tonemap_;
    };

} // namespace lumin::render

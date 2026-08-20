#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>

#include <nvrhi/nvrhi.h>

#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

namespace lumin::render {

    class VulkanContext;

    namespace detail {

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

    class PipelineManager {
    public:
        PipelineManager(VulkanContext& context, std::filesystem::path shaderDirectory);
        ~PipelineManager();

        PipelineManager(const PipelineManager&) = delete;
        PipelineManager& operator=(const PipelineManager&) = delete;

        /** deferred 使用 set 0/1，sky 使用 set 0/2 大气资源，其他全屏管线只使用 set 0。 */
        void create(nvrhi::BindingLayoutHandle fullscreenBindingLayout,
                    nvrhi::BindingLayoutHandle directLightingBindingLayout,
                    nvrhi::BindingLayoutHandle atmosphereBindingLayout, nvrhi::Format lightingFormat,
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

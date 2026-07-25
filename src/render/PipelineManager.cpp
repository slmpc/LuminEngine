#include "lumin/render/PipelineManager.hpp"

#include "lumin/render/VulkanContext.hpp"

#include <array>
#include <string>
#include <utility>

namespace lumin::render {

    PipelineManager::PipelineManager(VulkanContext& context, std::filesystem::path shaderDirectory)
        : shaders_(*context.rhiDevice().Get(), std::move(shaderDirectory)), factory_(*context.rhiDevice().Get()) {
    }

    PipelineManager::~PipelineManager() {
        destroy();
    }

    void PipelineManager::create(nvrhi::BindingLayoutHandle bindingLayout, nvrhi::Format lightingFormat,
                                 nvrhi::Format swapchainFormat) {
        destroy();

        const std::array<nvrhi::BindingLayoutHandle, 1> bindingLayouts = {std::move(bindingLayout)};
        auto createFullscreen = [&](const std::string& shaderName, nvrhi::Format colorFormat,
                                    nvrhi::GraphicsPipelineHandle& destination) {
            auto loadModule = [this](const std::string& moduleName, nvrhi::ShaderType type) {
                const std::string_view entryPoint = type == nvrhi::ShaderType::Vertex ? "vertexMain" : "fragmentMain";
                return shaders_.loadModule(moduleName, type, entryPoint);
            };
            auto createPipeline = [this, &destination](const GraphicsPipelineDesc& desc) {
                destination = factory_.createGraphicsPipeline(desc);
            };
            detail::createFullscreenPipeline(shaderName, colorFormat, bindingLayouts, loadModule, createPipeline);
        };

        try {
            createFullscreen("sky", lightingFormat, sky_);
            createFullscreen("deferred", lightingFormat, deferredLighting_);
            createFullscreen("taa", lightingFormat, taa_);
            createFullscreen("postprocess", swapchainFormat, tonemap_);
        } catch (...) {
            destroy();
            throw;
        }
    }

    void PipelineManager::destroy() noexcept {
        tonemap_ = nullptr;
        taa_ = nullptr;
        deferredLighting_ = nullptr;
        sky_ = nullptr;
    }

    const nvrhi::GraphicsPipelineHandle& PipelineManager::sky() const noexcept {
        return sky_;
    }

    const nvrhi::GraphicsPipelineHandle& PipelineManager::deferredLighting() const noexcept {
        return deferredLighting_;
    }

    const nvrhi::GraphicsPipelineHandle& PipelineManager::taa() const noexcept {
        return taa_;
    }

    const nvrhi::GraphicsPipelineHandle& PipelineManager::tonemap() const noexcept {
        return tonemap_;
    }

    const nvrhi::GraphicsPipelineHandle& PipelineManager::postprocess() const noexcept {
        return tonemap_;
    }

} // namespace lumin::render

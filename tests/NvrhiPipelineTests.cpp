#include "render/resources/FullscreenPipelineFactory.hpp"
#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <nvrhi/nvrhi.h>

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

    static_assert(std::constructible_from<lumin::render::ShaderLibrary, nvrhi::IDevice&, std::filesystem::path>);
    static_assert(requires(lumin::render::ShaderLibrary& shaders, lumin::render::ShaderId id) {
        { shaders.load(id) } -> std::same_as<nvrhi::ShaderHandle>;
    });

    static_assert(std::same_as<decltype(lumin::render::GraphicsPipelineDesc::vertexShader), nvrhi::ShaderHandle>);
    static_assert(std::same_as<decltype(lumin::render::GraphicsPipelineDesc::fragmentShader), nvrhi::ShaderHandle>);
    static_assert(std::same_as<decltype(lumin::render::GraphicsPipelineDesc::inputLayout), nvrhi::InputLayoutHandle>);
    static_assert(std::same_as<decltype(lumin::render::GraphicsPipelineDesc::bindingLayouts),
                               std::span<const nvrhi::BindingLayoutHandle>>);
    static_assert(
        std::same_as<decltype(lumin::render::GraphicsPipelineDesc::colorFormats), std::span<const nvrhi::Format>>);
    static_assert(std::same_as<decltype(lumin::render::GraphicsPipelineDesc::depthFormat), nvrhi::Format>);
    static_assert(std::same_as<decltype(lumin::render::GraphicsPipelineDesc::depthCompareOp), nvrhi::ComparisonFunc>);
    static_assert(std::same_as<decltype(lumin::render::GraphicsPipelineDesc::cullMode), nvrhi::RasterCullMode>);

    static_assert(std::constructible_from<lumin::render::PipelineFactory, nvrhi::IDevice&>);
    static_assert(requires(const lumin::render::PipelineFactory& factory,
                           const lumin::render::GraphicsPipelineDesc& desc) {
        { factory.createGraphicsPipeline(desc) } -> std::same_as<nvrhi::GraphicsPipelineHandle>;
    });

    static_assert(requires(const lumin::render::FullscreenPipelineFactory& factory,
                           std::span<const nvrhi::BindingLayoutHandle> layouts, nvrhi::Format format) {
        {
            factory.create(lumin::render::ShaderId::TaaVertex, lumin::render::ShaderId::TaaFragment, format, layouts)
        } -> std::same_as<nvrhi::GraphicsPipelineHandle>;
    });

    std::string spirvEntryPoint(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("Failed to open SPIR-V module: " + path.string());
        }
        const auto byteCount = static_cast<std::size_t>(input.tellg());
        if (byteCount < 5 * sizeof(std::uint32_t) || byteCount % sizeof(std::uint32_t) != 0) {
            throw std::runtime_error("Invalid SPIR-V module size: " + path.string());
        }

        std::vector<std::uint32_t> words(byteCount / sizeof(std::uint32_t));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(byteCount));
        if (!input || words.front() != 0x07230203u) {
            throw std::runtime_error("Invalid SPIR-V module header: " + path.string());
        }

        for (std::size_t offset = 5; offset < words.size();) {
            const std::uint32_t instruction = words[offset];
            const std::size_t wordCount = instruction >> 16;
            const std::uint32_t opcode = instruction & 0xffffu;
            if (wordCount == 0 || offset + wordCount > words.size()) {
                throw std::runtime_error("Malformed SPIR-V instruction stream: " + path.string());
            }
            if (opcode == 15u && wordCount >= 4) {
                const char* name = reinterpret_cast<const char*>(words.data() + offset + 3);
                const std::string_view encoded(name, (wordCount - 3) * sizeof(std::uint32_t));
                return std::string(encoded.substr(0, encoded.find('\0')));
            }
            offset += wordCount;
        }
        throw std::runtime_error("SPIR-V module has no entry point: " + path.string());
    }

    void requireEntryPoint(const char* fileName, const char* expected) {
        const std::filesystem::path path = std::filesystem::path(LUMIN_TEST_SHADER_DIR) / fileName;
        const std::string actual = spirvEntryPoint(path);
        if (actual != expected) {
            throw std::runtime_error(path.string() + " exports entry point '" + actual + "', expected '" + expected +
                                     "'.");
        }
    }

    void testFullscreenPipelineDescriptorIsWindingIndependent() {
        const std::array<nvrhi::BindingLayoutHandle, 1> layouts = {nullptr};
        std::vector<nvrhi::RasterCullMode> cullModes;
        auto loadModule = [](lumin::render::ShaderId) {
            return nvrhi::ShaderHandle{};
        };
        auto createPipeline = [&cullModes](const lumin::render::GraphicsPipelineDesc& desc) {
            cullModes.push_back(desc.cullMode);
        };

        lumin::render::detail::createFullscreenPipeline(
            lumin::render::ShaderId::TaaVertex, lumin::render::ShaderId::TaaFragment, nvrhi::Format::RGBA16_FLOAT,
            layouts, loadModule, createPipeline);

        if (cullModes != std::vector{nvrhi::RasterCullMode::None}) {
            throw std::runtime_error(
                "Fullscreen pipeline descriptors must disable culling after the fullscreen winding change.");
        }
    }

} // namespace

int main() {
    try {
        requireEntryPoint("GBuffer.vert.spv", "vertexMain");
        requireEntryPoint("GBuffer.frag.spv", "fragmentMain");
        requireEntryPoint("ImGui.vert.spv", "vertexMain");
        requireEntryPoint("ImGui.frag.spv", "fragmentMain");
        requireEntryPoint("Bloom.vert.spv", "vertexMain");
        requireEntryPoint("Bloom.frag.spv", "fragmentMain");
        testFullscreenPipelineDescriptorIsWindingIndependent();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NvrhiPipeline FAIL: %s\n", error.what());
        return 1;
    }
}

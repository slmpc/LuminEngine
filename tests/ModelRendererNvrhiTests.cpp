#include "lumin/render/ModelRenderer.hpp"
#include "render/DescriptorIndexingLimits.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function>
    void requireThrows(Function&& function, const std::string& message) {
        bool rejected = false;
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            rejected = true;
        }
        require(rejected, message);
    }

    struct DrawRecorder {
        std::uint32_t graphicsStateCalls = 0;
        std::uint32_t indexedIndirectCalls = 0;
        std::uint32_t indirectOffset = 0;
        std::uint32_t indirectDrawCount = 0;

        void setGraphicsState(const nvrhi::GraphicsState&) {
            ++graphicsStateCalls;
        }

        void drawIndexedIndirect(std::uint32_t offset, std::uint32_t drawCount) {
            ++indexedIndirectCalls;
            indirectOffset = offset;
            indirectDrawCount = drawCount;
        }
    };

    struct ResourceRecorder {
        std::uint32_t bufferCreations = 0;
        std::uint32_t bindingLayoutCreations = 0;
        std::uint32_t bindingSetCreations = 0;
        std::uint32_t pipelineCreations = 0;
        std::array<lumin::render::detail::ModelRendererBindingItem, 5> gbufferItems{};
        std::array<lumin::render::detail::ModelRendererBindingItem, 2> shadowItems{};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> materialArrayElements;

        void create(const lumin::render::detail::DescriptorIndexingPlan& plan) {
            const auto contract = lumin::render::detail::makeModelRendererBindingContract(plan);
            gbufferItems = contract.gbufferItems;
            shadowItems = contract.shadowItems;
            bufferCreations += 3;
            bindingLayoutCreations += 2;
            bindingSetCreations += contract.gbufferSetCount + contract.shadowSetCount;
            pipelineCreations += 2;
            lumin::render::detail::forEachMaterialTextureArrayElement(
                contract, [&](std::uint32_t baseColorBinding, std::uint32_t normalRoughnessBinding,
                              std::uint32_t arrayElement) {
                    materialArrayElements.emplace_back(baseColorBinding, arrayElement);
                    materialArrayElements.emplace_back(normalRoughnessBinding, arrayElement);
                });
        }

        [[nodiscard]] std::uint32_t totalCreations() const {
            return bufferCreations + bindingLayoutCreations + bindingSetCreations + pipelineCreations;
        }
    };

    struct SlotWriteRecorder {
        std::vector<std::uint32_t> writtenSlots;

        void write(lumin::render::detail::FrameSlotReadiness& slots, std::uint32_t frameIndex) {
            slots.requireReady(frameIndex);
            writtenSlots.push_back(frameIndex);
        }
    };

    void testPublicNvRhiContract() {
        static_assert(std::same_as<typename lumin::render::ModelBatch::Command, nvrhi::DrawIndexedIndirectArguments>);
        static_assert(requires(lumin::render::VulkanContext& context, const lumin::scene::Level& level,
                               std::filesystem::path shaderDirectory, std::span<const nvrhi::Format> colorFormats) {
            lumin::render::ModelRenderer(context, level, shaderDirectory, colorFormats, nvrhi::Format::RGBA8_UNORM,
                                         nvrhi::Format::D32, 2, lumin::render::ModelRendererCapabilities{});
        });
        static_assert(requires(lumin::render::ModelRenderer& renderer, nvrhi::ICommandList& commandList,
                               nvrhi::IFramebuffer& framebuffer, const glm::mat4& matrix) {
            renderer.recordGBuffer(commandList, framebuffer, 1280, 720, 0, matrix, matrix);
            renderer.recordShadow(commandList, framebuffer, 2048, 2048, 0, 0, matrix);
        });
        static_assert(sizeof(lumin::render::ObjectData) == 224);
        static_assert(alignof(lumin::render::ObjectData) == 16);
        constexpr lumin::render::ModelRendererCapabilities defaults;
        static_assert(defaults.maxMaterialTextureArrayLength < std::numeric_limits<std::uint16_t>::max());
        static_assert(defaults.maxDrawIndirectCount < std::numeric_limits<std::uint32_t>::max());
        static_assert(defaults.maxImageDimension2D < std::numeric_limits<std::uint32_t>::max());
    }

    void testRecordedGBufferAndShadowContract() {
        nvrhi::GraphicsState state;
        constexpr std::uint32_t batchDrawCount = 3;
        DrawRecorder gbuffer;
        DrawRecorder shadow;
        lumin::render::detail::recordModelIndexedIndirect(gbuffer, state, batchDrawCount);
        lumin::render::detail::recordModelIndexedIndirect(shadow, state, batchDrawCount);
        require(gbuffer.graphicsStateCalls == 1 && gbuffer.indexedIndirectCalls == 1,
                "G-buffer recording must issue exactly one indexed indirect draw.");
        require(shadow.graphicsStateCalls == 1 && shadow.indexedIndirectCalls == 1,
                "Shadow recording must issue exactly one indexed indirect draw.");
        require(gbuffer.indirectOffset == 0 && shadow.indirectOffset == 0 &&
                    gbuffer.indirectDrawCount == batchDrawCount && shadow.indirectDrawCount == batchDrawCount,
                "Each pass must draw the packed batch command count from offset zero.");
    }

    void testViewportExtentContract() {
        const std::filesystem::path sourcePath =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "src/render/ModelRenderer.cpp";
        std::ifstream source(sourcePath, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        require(contents.find("nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height))") !=
                    std::string::npos,
                "ModelRenderer graphics state must set one viewport and scissor from the requested extent.");
        require(contents.find("recordGBuffer(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,\n"
                              "                                      std::uint32_t width, std::uint32_t height") !=
                    std::string::npos,
                "G-buffer recording must accept the current frame extent.");
        require(contents.find("recordShadow(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,\n"
                              "                                     std::uint32_t width, std::uint32_t height") !=
                    std::string::npos,
                "Shadow recording must accept the shadow-map extent.");
    }

    void testStaticBufferStateContract() {
        const std::filesystem::path sourcePath =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "src/render/ModelRenderer.cpp";
        std::ifstream source(sourcePath, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        for (const char* state : {"ResourceStates::VertexBuffer", "ResourceStates::IndexBuffer",
                                  "ResourceStates::IndirectArgument"}) {
            require(contents.find(state) != std::string::npos,
                    "Every uploaded model buffer must declare its stable draw-time state.");
        }
    }

    void testRecordedCreationContract() {
        lumin::render::ModelRendererCapabilities capabilities{
            .maxMaterialTextureArrayLength = 4,
            .maxDrawIndirectCount = 3,
            .maxImageDimension2D = 1024,
        };
        ResourceRecorder recorder;
        (void)lumin::render::detail::createModelRendererResourcesAfterPreflight(
            capabilities, 2, 2, 4, 3,
            [&](const lumin::render::detail::DescriptorIndexingPlan& plan) { recorder.create(plan); });

        require(recorder.gbufferItems[0].kind == lumin::render::detail::ModelRendererBindingKind::ConstantBuffer &&
                    recorder.gbufferItems[0].binding == 0 &&
                    recorder.gbufferItems[1].kind == lumin::render::detail::ModelRendererBindingKind::StructuredBuffer &&
                    recorder.gbufferItems[1].binding == 1 &&
                    recorder.gbufferItems[2].kind == lumin::render::detail::ModelRendererBindingKind::Texture &&
                    recorder.gbufferItems[2].binding == 2 && recorder.gbufferItems[2].arrayLength == 3 &&
                    recorder.gbufferItems[3].kind == lumin::render::detail::ModelRendererBindingKind::Texture &&
                    recorder.gbufferItems[3].binding == 3 && recorder.gbufferItems[3].arrayLength == 3 &&
                    recorder.gbufferItems[4].kind == lumin::render::detail::ModelRendererBindingKind::Sampler &&
                    recorder.gbufferItems[4].binding == 4,
                "G-buffer layout must retain bindings 0 through 4 and both material arrays.");
        require(recorder.shadowItems[0].binding == 0 && recorder.shadowItems[1].binding == 1,
                "Shadow layout must retain bindings 0 and 1.");
        require(recorder.bindingSetCreations == 10 && recorder.materialArrayElements.size() == 6,
                "Two frame slots and four cascades must create ten sets and all material array elements.");
        for (std::uint32_t textureIndex = 0; textureIndex < 3; ++textureIndex) {
            require(recorder.materialArrayElements[textureIndex * 2] == std::pair{2U, textureIndex} &&
                        recorder.materialArrayElements[textureIndex * 2 + 1] == std::pair{3U, textureIndex},
                    "Each material array element must bind base-color and normal-roughness textures.");
        }

        ResourceRecorder rejected;
        capabilities.maxMaterialTextureArrayLength = 2;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::createModelRendererResourcesAfterPreflight(
                    capabilities, 2, 2, 4, 3,
                    [&](const lumin::render::detail::DescriptorIndexingPlan& plan) { rejected.create(plan); });
            },
            "Capacity rejection must occur before any resource, binding, or pipeline creation.");
        require(rejected.totalCreations() == 0,
                "Rejected capacity must leave recorder resource, binding, and pipeline creation counters at zero.");

        const std::array oversizedImage = {
            lumin::render::detail::ModelRendererMaterialImageDimensions{1025, 1},
        };
        ResourceRecorder imageRejected;
        capabilities.maxMaterialTextureArrayLength = 4;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::createModelRendererResourcesAfterPreflight(
                    capabilities, 1, 2, 4, 1, oversizedImage,
                    [&](const lumin::render::detail::DescriptorIndexingPlan& plan) { imageRejected.create(plan); });
            },
            "Image rejection must occur before any resource, binding, or pipeline creation.");
        require(imageRejected.totalCreations() == 0,
                "Rejected image dimensions must leave all creation counters at zero.");

        ResourceRecorder drawRejected;
        capabilities.maxDrawIndirectCount = 2;
        requireThrows<std::length_error>(
            [&] {
                (void)lumin::render::detail::createModelRendererResourcesAfterPreflight(
                    capabilities, 1, 2, 4, 3,
                    [&](const lumin::render::detail::DescriptorIndexingPlan& plan) { drawRejected.create(plan); });
            },
            "Draw-count rejection must occur before any resource, binding, or pipeline creation.");
        require(drawRejected.totalCreations() == 0,
                "Rejected draw count must leave all creation counters at zero.");
    }

    void testFrameSlotsAndTopologyReset() {
        lumin::render::detail::FrameSlotReadiness slots(2);
        SlotWriteRecorder recorder;
        requireThrows<std::logic_error>([&] { recorder.write(slots, 0); },
                                        "Slot zero writes must wait for slot zero readiness.");
        requireThrows<std::logic_error>([&] { recorder.write(slots, 1); },
                                        "Slot one writes must wait for slot one readiness.");
        slots.markReady(0);
        recorder.write(slots, 0);
        slots.markReady(1);
        recorder.write(slots, 1);
        require(recorder.writtenSlots == std::vector<std::uint32_t>{0, 1},
                "Slot zero and slot one writes must follow their matching ready events.");
        slots.consumeReady(0);
        requireThrows<std::logic_error>([&] { recorder.write(slots, 0); },
                                        "Consumed slot readiness must not permit a stale write.");

        require(lumin::render::detail::requiresPreviousModelReset(false, false),
                "A rebuilt renderer must reset previous models before its first sync.");
        require(!lumin::render::detail::requiresPreviousModelReset(false, true),
                "A stable topology must retain previous models between syncs.");
        require(lumin::render::detail::requiresPreviousModelReset(true, true),
                "An explicit motion reset must reset previous models.");
    }

    void testMalformedCapacityInputs() {
        const lumin::render::ModelRendererCapabilities capabilities{
            .maxMaterialTextureArrayLength = 4,
            .maxDrawIndirectCount = 4,
            .maxImageDimension2D = 64,
        };
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::detail::createModelRendererResourcesAfterPreflight(
                    capabilities, 1, 0, 4, 1, [](const auto&) {});
            },
            "Zero frame slots must be rejected before creation.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::detail::createModelRendererResourcesAfterPreflight(
                    capabilities, 1, 1, 0, 1, [](const auto&) {});
            },
            "Zero cascades must be rejected before creation.");
        requireThrows<std::invalid_argument>(
            [&] {
                lumin::render::detail::validateMaterialImageDimensions(
                    lumin::render::detail::toDescriptorIndexingLimits(capabilities), 0, 1);
            },
            "Zero image dimensions must be rejected before creation.");
    }

}

int main() {
    try {
        testPublicNvRhiContract();
        testRecordedGBufferAndShadowContract();
        testViewportExtentContract();
        testStaticBufferStateContract();
        testRecordedCreationContract();
        testFrameSlotsAndTopologyReset();
        testMalformedCapacityInputs();
        std::cout << "PASS: ModelRenderer capacity, binding, slot, topology, and MDI recorder contract.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}

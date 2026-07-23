#include "lumin/render/gi/GlobalIllumination.hpp"
#include "lumin/render/gi/SsaoBackend.hpp"
#include "lumin/scene/Level.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

    using lumin::render::FrameGraph;
    using lumin::render::FrameGraphBuilder;
    using lumin::render::FrameGraphContext;
    using lumin::render::FrameGraphPassType;
    using lumin::render::FrameGraphResourceHandle;
    using lumin::render::FrameGraphTextureDesc;
    using lumin::render::gi::BackendInfo;
    using lumin::render::gi::CreateInfo;
    using lumin::render::gi::FrameInfo;
    using lumin::render::gi::GlobalIlluminationBackend;

    static_assert(!std::is_copy_constructible_v<GlobalIlluminationBackend>);
    static_assert(!std::is_move_constructible_v<GlobalIlluminationBackend>);

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    class FakeGlobalIlluminationBackend final : public GlobalIlluminationBackend {
    public:
        [[nodiscard]] BackendInfo info() const noexcept override {
            return BackendInfo{"Fake GI", true, false};
        }

        void create(const CreateInfo&) override {
        }

        void destroy() noexcept override {
        }

        void invalidateHistory() noexcept override {
            historyInvalidated = true;
        }

        void addPasses(FrameGraph& frameGraph, const FrameInfo& frameInfo) override {
            frameGraph.addPass(
                "Fake GI", FrameGraphPassType::Graphics,
                [position = frameInfo.position, normal = frameInfo.normalRoughness,
                 output = frameInfo.output](FrameGraphBuilder& builder) {
                    builder.readTexture(position, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                    builder.readTexture(normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                    builder.writeTexture(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                },
                [this](const FrameGraphContext&) {
                    executionOrder->push_back("GI");
                });
        }

        std::vector<std::string>* executionOrder = nullptr;
        bool historyInvalidated = false;
    };

    void testDisabledNeutralOutputPolicy() {
        require(lumin::render::gi::neutralOutput[lumin::render::gi::indirectRadianceFirstChannel] == 0.0f &&
                    lumin::render::gi::neutralOutput[1] == 0.0f && lumin::render::gi::neutralOutput[2] == 0.0f,
                "GI disabled must contribute zero indirect radiance.");
        require(lumin::render::gi::neutralOutput[lumin::render::gi::ambientVisibilityChannel] == 1.0f,
                "GI disabled must preserve full ambient visibility in the neutral output.");
    }

    void testSsaoBackendInfo() {
        const std::unique_ptr<GlobalIlluminationBackend> backend = lumin::render::gi::makeSsaoBackend("unused");
        const BackendInfo info = backend->info();
        require(info.name == "SSAO", "The default GI backend must identify itself as SSAO.");
        require(!info.temporal, "The current SSAO backend must not claim temporal history.");
        require(!info.hardwareRayTracing, "The current SSAO backend must not claim hardware ray tracing.");
    }

    void testFrameGraphOrdering() {
        FrameGraph frameGraph;
        FrameGraphTextureDesc texture;
        texture.width = 16;
        texture.height = 16;
        texture.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        texture.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        const FrameGraphResourceHandle position = frameGraph.importTexture("gbuffer.position", texture);
        const FrameGraphResourceHandle normal = frameGraph.importTexture("gbuffer.normal", texture);
        const FrameGraphResourceHandle albedo = frameGraph.importTexture("gbuffer.albedo", texture);
        const FrameGraphResourceHandle motion = frameGraph.importTexture("gbuffer.motion", texture);
        const FrameGraphResourceHandle depth = frameGraph.importTexture("gbuffer.depth", texture);
        const FrameGraphResourceHandle output = frameGraph.importTexture("gi.output", texture);

        std::vector<std::string> order;
        frameGraph.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [position, normal](FrameGraphBuilder& builder) {
                builder.writeTexture(position, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                builder.writeTexture(normal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            },
            [&order](const FrameGraphContext&) {
                order.push_back("G-buffer");
            });

        lumin::scene::Level level;
        FakeGlobalIlluminationBackend backend;
        backend.executionOrder = &order;
        const FrameInfo frameInfo{level,    0,      0,      true,   false, VkExtent2D{16, 16},
                                  position, normal, albedo, motion, depth, output};
        backend.addPasses(frameGraph, frameInfo);

        frameGraph.addPass(
            "Deferred lighting", FrameGraphPassType::Graphics,
            [output](FrameGraphBuilder& builder) {
                builder.readTexture(output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            },
            [&order](const FrameGraphContext&) {
                order.push_back("Deferred");
            });

        frameGraph.execute(FrameGraphContext{});
        require(order == std::vector<std::string>{"G-buffer", "GI", "Deferred"},
                "FrameGraph must order G-buffer before GI and GI before deferred lighting.");
    }

    void testHistoryInvalidationPolicy() {
        using lumin::render::gi::HistoryInvalidationState;
        using lumin::render::gi::shouldInvalidateHistory;

        require(!shouldInvalidateHistory(HistoryInvalidationState{}), "A stable frame must not invalidate GI history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.cameraCut = true}),
                "A camera cut must invalidate GI history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.topologyChanged = true}),
                "A topology change must invalidate GI history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.backendReenabled = true}),
                "Re-enabling GI must invalidate backend history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.swapchainRecreated = true}),
                "Swapchain recreation must invalidate GI history.");
    }

    std::string readSource(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Unable to open GI shader source: " + path.string());
        }
        return std::string(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    }

    void testShaderOutputContract() {
#if defined(LUMIN_TEST_SOURCE_DIR)
        const std::filesystem::path sourceDirectory = LUMIN_TEST_SOURCE_DIR;
#else
        const std::filesystem::path sourceDirectory = ".";
#endif
        const std::string ssao = readSource(sourceDirectory / "shaders" / "ssao.slang");
        require(ssao.find("frame.renderOptions.y < 0.5") != std::string::npos,
                "SSAO must retain the enabled-path guard.");
        require(ssao.find("return float4(0.0, 0.0, 0.0, 1.0);") != std::string::npos,
                "Disabled SSAO must emit the neutral packed GI output.");
        require(ssao.find("return float4(0.0, 0.0, 0.0, ao);") != std::string::npos,
                "Enabled SSAO must pack ambient visibility in GI alpha.");

        const std::string deferred = readSource(sourceDirectory / "shaders" / "deferred.slang");
        require(deferred.find("Texture2D<float4> globalIlluminationTexture") != std::string::npos,
                "Deferred lighting must consume the packed RGBA GI output.");
        require(deferred.find("legacyAmbient * globalIllumination.a + globalIllumination.rgb") != std::string::npos,
                "Deferred lighting must combine ambient visibility and indirect radiance.");
    }

} // namespace

int main() {
    try {
        testDisabledNeutralOutputPolicy();
        testSsaoBackendInfo();
        testFrameGraphOrdering();
        testHistoryInvalidationPolicy();
        testShaderOutputContract();
        std::cout << "GlobalIllumination PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GlobalIllumination FAIL: " << error.what() << '\n';
        return 1;
    }
}

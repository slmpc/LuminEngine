#include "render/gi/GiComposite.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using lumin::render::FrameGraph;
    using lumin::render::FrameGraphBufferDesc;
    using lumin::render::FrameGraphContext;
    using lumin::render::FrameGraphPassType;
    using lumin::render::FrameGraphTextureDesc;
    using lumin::render::gi::GiCompositeGraphResources;
    using lumin::render::gi::GiCompositeResources;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Callable> void requireThrows(Callable&& callable, const char* message) {
        try {
            callable();
        } catch (const Exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    [[nodiscard]] bool nearlyEqual(float left, float right, float epsilon = 1.0e-5F) {
        return std::abs(left - right) <= epsilon;
    }

    [[nodiscard]] bool nearlyEqual(const glm::vec3& left, const glm::vec3& right, float epsilon = 1.0e-5F) {
        return nearlyEqual(left.x, right.x, epsilon) && nearlyEqual(left.y, right.y, epsilon) &&
               nearlyEqual(left.z, right.z, epsilon);
    }

    class FakeTexture final : public nvrhi::RefCounter<nvrhi::ITexture> {
    public:
        explicit FakeTexture(nvrhi::TextureDesc desc) : desc_(std::move(desc)) {
        }

        [[nodiscard]] const nvrhi::TextureDesc& getDesc() const override {
            return desc_;
        }

        nvrhi::Object getNativeView(nvrhi::ObjectType, nvrhi::Format, nvrhi::TextureSubresourceSet,
                                    nvrhi::TextureDimension, bool) override {
            return nullptr;
        }

    private:
        nvrhi::TextureDesc desc_;
    };

    class FakeBuffer final : public nvrhi::RefCounter<nvrhi::IBuffer> {
    public:
        explicit FakeBuffer(nvrhi::BufferDesc desc) : desc_(std::move(desc)) {
        }

        [[nodiscard]] const nvrhi::BufferDesc& getDesc() const override {
            return desc_;
        }

        [[nodiscard]] nvrhi::GpuVirtualAddress getGpuVirtualAddress() const override {
            return 0;
        }

    private:
        nvrhi::BufferDesc desc_;
    };

    [[nodiscard]] nvrhi::TextureHandle makeTexture(nvrhi::Format format, bool uav = false) {
        nvrhi::TextureDesc desc;
        desc.width = 65;
        desc.height = 33;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.format = format;
        desc.isShaderResource = true;
        desc.isUAV = uav;
        desc.initialState = nvrhi::ResourceStates::Common;
        return nvrhi::TextureHandle::Create(new FakeTexture(std::move(desc)));
    }

    [[nodiscard]] nvrhi::BufferHandle makeMaterialBuffer(std::uint32_t count = 3) {
        nvrhi::BufferDesc desc;
        desc.byteSize = sizeof(lumin::render::gpu::GpuMaterialData) * count;
        desc.structStride = sizeof(lumin::render::gpu::GpuMaterialData);
        desc.initialState = nvrhi::ResourceStates::Common;
        return nvrhi::BufferHandle::Create(new FakeBuffer(std::move(desc)));
    }

    [[nodiscard]] GiCompositeResources makeResources() {
        return {
            .diffuseRadianceHitDistance = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .specularRadianceHitDistance = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .position = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .normalRoughness = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .albedoMetallic = makeTexture(nvrhi::Format::RGBA8_UNORM),
            .materialId = makeTexture(nvrhi::Format::R32_UINT),
            .materials = makeMaterialBuffer(),
            .globalIllumination = makeTexture(nvrhi::Format::RGBA16_FLOAT, true),
        };
    }

    void testConstantsAndDispatchContract() {
        const nvrhi::BufferDesc constants = lumin::render::gi::detail::makeGiCompositeConstantBufferDesc();
        require(constants.byteSize == sizeof(lumin::render::gi::GiCompositeConstants) && constants.isConstantBuffer &&
                    constants.cpuAccess == nvrhi::CpuAccessMode::Write &&
                    constants.initialState == nvrhi::ResourceStates::ConstantBuffer && !constants.keepInitialState,
                "GI composite constants must be frame-slot-local and remain in ConstantBuffer state.");

        const auto dispatch = lumin::render::gi::detail::makeGiCompositeDispatchSize({65, 33});
        require(dispatch == lumin::render::gi::GiCompositeDispatchSize{9, 5, 1},
                "GI composite must round an odd extent up to complete 8x8 groups.");
        requireThrows<std::invalid_argument>(
            [] {
                (void)lumin::render::gi::detail::makeGiCompositeDispatchSize({0, 33});
            },
            "GI composite must reject an empty dispatch.");

        const lumin::render::gi::GiCompositeFrameParameters parameters{
            .frameSlot = lumin::render::core::FrameSlotIndex{1},
            .extent = {65, 33},
            .cameraPosition = {2.0F, 3.0F, 4.0F},
            .frameSlotFenceWaited = true,
        };
        const auto packed = lumin::render::gi::detail::makeGiCompositeConstants(parameters, 7);
        require(packed.cameraPosition == glm::vec4(2.0F, 3.0F, 4.0F, 1.0F) &&
                    packed.renderInfo == glm::uvec4(65U, 33U, 7U, 0xffffffffU),
                "GI composite CPU constants must match the shader ABI.");
    }

    void testBindingAbiAndResourceValidation() {
        const nvrhi::BindingLayoutDesc layout = lumin::render::gi::detail::makeGiCompositeBindingLayoutDesc();
        require(layout.visibility == nvrhi::ShaderType::Compute && layout.registerSpace == 0 &&
                    layout.registerSpaceIsDescriptorSet && layout.bindings.size() == 9,
                "GI composite must expose one explicit compute descriptor set.");
        for (std::uint32_t binding = 0; binding < 6; ++binding) {
            require(layout.bindings[binding] == nvrhi::BindingLayoutItem::Texture_SRV(binding),
                    "GI composite bindings 0 through 5 must be texture SRVs.");
        }
        require(layout.bindings[6] == nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6) &&
                    layout.bindings[7] == nvrhi::BindingLayoutItem::ConstantBuffer(7) &&
                    layout.bindings[8] == nvrhi::BindingLayoutItem::Texture_UAV(8),
                "GI composite material, constants, and output bindings differ from the Slang ABI.");

        GiCompositeResources resources = makeResources();
        const nvrhi::BufferHandle constants =
            nvrhi::BufferHandle::Create(new FakeBuffer(lumin::render::gi::detail::makeGiCompositeConstantBufferDesc()));
        require(lumin::render::gi::detail::validateGiCompositeResources(resources, {65, 33}) == 3,
                "GI composite must derive a bounded material count from GpuMaterialData stride.");

        const nvrhi::BindingSetDesc bindings =
            lumin::render::gi::detail::makeGiCompositeBindingSetDesc(resources, constants);
        require(bindings.bindings.size() == 9 && bindings.bindings[0].slot == 0 &&
                    bindings.bindings[0].resourceHandle == resources.diffuseRadianceHitDistance &&
                    bindings.bindings[5].slot == 5 && bindings.bindings[5].resourceHandle == resources.materialId &&
                    bindings.bindings[6].type == nvrhi::ResourceType::StructuredBuffer_SRV &&
                    bindings.bindings[7].type == nvrhi::ResourceType::ConstantBuffer &&
                    bindings.bindings[8].type == nvrhi::ResourceType::Texture_UAV &&
                    bindings.bindings[8].resourceHandle == resources.globalIllumination,
                "GI composite binding set must retain every exact physical resource.");

        resources.materialId = makeTexture(nvrhi::Format::RGBA8_UNORM);
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::gi::detail::validateGiCompositeResources(resources, {65, 33});
            },
            "GI composite must reject a non-R32_UINT material ID texture.");

        resources = makeResources();
        nvrhi::TextureDesc outputDesc = resources.globalIllumination->getDesc();
        outputDesc.isShaderResource = false;
        resources.globalIllumination = nvrhi::TextureHandle::Create(new FakeTexture(std::move(outputDesc)));
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::gi::detail::validateGiCompositeResources(resources, {65, 33});
            },
            "Packed GI output must remain both UAV-writable and shader-readable for deferred lighting.");
    }

    void testBothMaterialModelsModulateUncoloredNrdSignals() {
        lumin::render::gpu::GpuMaterialData blinn;
        blinn.metadata.x = 1U;
        blinn.specularColorShininess = {0.2F, 0.4F, 0.8F, 64.0F};
        const glm::vec3 blinnResult =
            lumin::render::gi::detail::modulateGiRadiance({2.0F, 2.0F, 2.0F}, {0.5F, 0.5F, 0.5F}, {0.5F, 0.25F, 0.1F},
                                                          0.0F, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F}, blinn);
        require(nearlyEqual(blinnResult, {1.1F, 0.7F, 0.6F}),
                "Blinn-Phong GI must apply base color and explicit specular color after NRD.");

        lumin::render::gpu::GpuMaterialData pbr;
        pbr.metadata.x = 0U;
        const glm::vec3 dielectricResult =
            lumin::render::gi::detail::modulateGiRadiance({1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, {0.8F, 0.5F, 0.2F},
                                                          0.0F, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F}, pbr);
        require(nearlyEqual(dielectricResult, {0.808F, 0.52F, 0.232F}),
                "PBR dielectric GI must apply energy-conserving diffuse and F0 specular modulation.");

        const glm::vec3 metalResult =
            lumin::render::gi::detail::modulateGiRadiance({10.0F, 10.0F, 10.0F}, {1.0F, 1.0F, 1.0F}, {0.8F, 0.5F, 0.2F},
                                                          1.0F, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F}, pbr);
        require(nearlyEqual(metalResult, {0.8F, 0.5F, 0.2F}),
                "PBR metals must suppress diffuse GI and tint specular GI with base reflectance.");
    }

    class DispatchProbe {
    public:
        void setComputeState(const nvrhi::ComputeState& state) {
            stateSet = &state;
        }

        void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            dispatched = {x, y, z};
        }

        const nvrhi::ComputeState* stateSet = nullptr;
        lumin::render::gi::GiCompositeDispatchSize dispatched;
    };

    class BarrierRecorder final : public lumin::render::FrameGraphBarrierRecorder {
    public:
        void beginTrackingTextureState(nvrhi::ITexture*, nvrhi::TextureSubresourceSet, nvrhi::ResourceStates) override {
        }

        void beginTrackingBufferState(nvrhi::IBuffer*, nvrhi::ResourceStates) override {
        }

        void setTextureState(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet,
                             nvrhi::ResourceStates state) override {
            textureStates.emplace_back(texture, state);
        }

        void setBufferState(nvrhi::IBuffer* buffer, nvrhi::ResourceStates state) override {
            bufferStates.emplace_back(buffer, state);
        }

        void setAccelerationStructureState(nvrhi::rt::IAccelStruct*, nvrhi::ResourceStates) override {
        }

        void commitBarriers() override {
        }

        std::vector<std::pair<nvrhi::ITexture*, nvrhi::ResourceStates>> textureStates;
        std::vector<std::pair<nvrhi::IBuffer*, nvrhi::ResourceStates>> bufferStates;
    };

    void testDispatchAndFrameGraphDeclarations() {
        DispatchProbe probe;
        nvrhi::ComputeState state;
        lumin::render::gi::detail::recordGiCompositeDispatch(probe, state, {9, 5, 1});
        require(probe.stateSet == &state && probe.dispatched == lumin::render::gi::GiCompositeDispatchSize{9, 5, 1},
                "GI composite must bind compute state before dispatch.");

        GiCompositeResources native = makeResources();
        const nvrhi::BufferHandle constants =
            nvrhi::BufferHandle::Create(new FakeBuffer(lumin::render::gi::detail::makeGiCompositeConstantBufferDesc()));
        FrameGraph graph;
        const auto importTexture = [&](const char* name, nvrhi::ITexture* texture) {
            return graph.importTexture(name, FrameGraphTextureDesc{.texture = texture});
        };
        GiCompositeGraphResources resources{
            .diffuseRadianceHitDistance = importTexture("diffuse", native.diffuseRadianceHitDistance),
            .specularRadianceHitDistance = importTexture("specular", native.specularRadianceHitDistance),
            .position = importTexture("position", native.position),
            .normalRoughness = importTexture("normal", native.normalRoughness),
            .albedoMetallic = importTexture("albedo", native.albedoMetallic),
            .materialId = importTexture("material-id", native.materialId),
            .materials =
                graph.importBuffer("materials", FrameGraphBufferDesc{.size = native.materials->getDesc().byteSize,
                                                                     .buffer = native.materials}),
            .globalIllumination = importTexture("gi-output", native.globalIllumination),
        };
        const auto constantsResource = graph.importBuffer(
            "constants", FrameGraphBufferDesc{.size = constants->getDesc().byteSize,
                                              .buffer = constants,
                                              .initialState = nvrhi::ResourceStates::ConstantBuffer});
        std::vector<std::string> executed;
        const auto dependency =
            graph.addPass("nrd-last", FrameGraphPassType::Compute, {}, [&](const FrameGraphContext&) {
                executed.emplace_back("nrd");
            });
        const auto composite = lumin::render::gi::detail::addGiCompositePass(graph, resources, constantsResource,
                                                                             dependency, [&](const FrameGraphContext&) {
                                                                                 executed.emplace_back("composite");
                                                                             });
        graph.addPass(
            "deferred-consumer", FrameGraphPassType::Graphics,
            [output = resources.globalIllumination](lumin::render::FrameGraphBuilder& builder) {
                builder.readTexture(output, nvrhi::ResourceStates::ShaderResource);
            },
            [&](const FrameGraphContext&) {
                executed.emplace_back("deferred");
            });
        graph.compile();
        require(graph.passName(composite) == "gi-composite" && graph.executionOrder().size() == 3,
                "GI composite must register one named pass between NRD and its consumer.");

        BarrierRecorder barriers;
        graph.execute(FrameGraphContext{.barriers = &barriers});
        require(executed == std::vector<std::string>{"nrd", "composite", "deferred"},
                "GI composite must honor the final NRD pass and output read dependency.");
        require(barriers.textureStates.size() == 8 && barriers.bufferStates.size() == 1,
                "GI composite must transition all six texture inputs, material table, and output UAV.");
        require(barriers.textureStates[6].first == native.globalIllumination &&
                    barriers.textureStates[6].second == nvrhi::ResourceStates::UnorderedAccess &&
                    barriers.textureStates[7].first == native.globalIllumination &&
                    barriers.textureStates[7].second == nvrhi::ResourceStates::ShaderResource &&
                    barriers.bufferStates[0].first == native.materials &&
                    barriers.bufferStates[0].second == nvrhi::ResourceStates::ShaderResource,
                "FrameGraph must own the composite UAV/readback and material buffer barriers.");
    }

    void testShaderWritesNeutralBackgroundAndReplacementAlpha() {
        const std::filesystem::path shaderPath =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "shaders/gi_composite.slang";
        std::ifstream shader(shaderPath, std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>(shader), std::istreambuf_iterator<char>()};
        require(source.find("#include \"NRD.hlsli\"") != std::string::npos &&
                    source.find("REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packedSignal)") != std::string::npos &&
                    source.find("unpackReblurRadiance(denoisedDiffuseRadianceHitDistance.Load") != std::string::npos &&
                    source.find("unpackReblurRadiance(denoisedSpecularRadianceHitDistance.Load") != std::string::npos &&
                    source.find("globalIlluminationOutput[pixel] = kNeutralOutput") != std::string::npos &&
                    source.find("material.metadata.x == kBlinnPhongSurfaceModel") != std::string::npos &&
                    source.find("diffuseRadiance * albedo * diffuseWeight + specularRadiance * fresnel") !=
                        std::string::npos &&
                    source.find("float4(indirectRadiance, 0.0)") != std::string::npos,
                "GI composite shader must decode REBLUR signals before both material modulation paths.");
    }

} // namespace

int main() {
    try {
        testConstantsAndDispatchContract();
        testBindingAbiAndResourceValidation();
        testBothMaterialModelsModulateUncoloredNrdSignals();
        testDispatchAndFrameGraphDeclarations();
        testShaderWritesNeutralBackgroundAndReplacementAlpha();
        std::puts("GiComposite PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GiComposite FAIL: %s\n", error.what());
        return 1;
    }
}

#include "render/gi/raytracing/RtDiNrdInputs.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

    using lumin::render::gi::RtDiNrdInputResources;
    using lumin::render::gi::RtDiNrdSignalResources;

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

    [[nodiscard]] RtDiNrdInputResources makeInputs() {
        return {
            .diffuseRadianceHitT = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .specularRadianceHitT = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .position = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .normalRoughness = makeTexture(nvrhi::Format::RGBA16_FLOAT),
            .albedoMetallic = makeTexture(nvrhi::Format::RGBA8_UNORM),
            .materialId = makeTexture(nvrhi::Format::R32_UINT),
            .viewZ = makeTexture(nvrhi::Format::R32_FLOAT),
            .motion = makeTexture(nvrhi::Format::RG16_FLOAT),
            .materials = makeMaterialBuffer(),
        };
    }

    [[nodiscard]] RtDiNrdSignalResources makeOutputs() {
        return {
            .diffuseRadianceHitDistance = makeTexture(nvrhi::Format::RGBA16_FLOAT, true),
            .specularRadianceHitDistance = makeTexture(nvrhi::Format::RGBA16_FLOAT, true),
            .viewZ = makeTexture(nvrhi::Format::R32_FLOAT, true),
            .normalRoughness = makeTexture(nvrhi::Format::R10G10B10A2_UNORM, true),
            .motion = makeTexture(nvrhi::Format::RG16_FLOAT, true),
        };
    }

    void testSignalFormatsAndTextureContract() {
        const lumin::render::gi::RtDiNrdSignalFormats formats;
        require(formats.diffuseRadianceHitDistance == nvrhi::Format::RGBA16_FLOAT &&
                    formats.specularRadianceHitDistance == nvrhi::Format::RGBA16_FLOAT &&
                    formats.viewZ == nvrhi::Format::R32_FLOAT &&
                    formats.normalRoughness == nvrhi::Format::R10G10B10A2_UNORM &&
                    formats.motion == nvrhi::Format::RG16_FLOAT,
                "RTDI preparation signals must use the formats required by NRD REBLUR.");

        const nvrhi::TextureDesc desc = lumin::render::gi::detail::makeRtDiNrdSignalTextureDesc(
            1920, 1080, formats.diffuseRadianceHitDistance, "RTDI diffuse");
        require(desc.width == 1920 && desc.height == 1080 && desc.format == nvrhi::Format::RGBA16_FLOAT &&
                    desc.isShaderResource && desc.isUAV && !desc.keepInitialState &&
                    desc.initialState == nvrhi::ResourceStates::Common,
                "RTDI preparation outputs must support FrameGraph-owned SRV/UAV transitions.");
        requireThrows<std::invalid_argument>(
            [] {
                (void)lumin::render::gi::detail::makeRtDiNrdSignalTextureDesc(
                    0, 1080, nvrhi::Format::RGBA16_FLOAT, "invalid");
            },
            "RTDI preparation must reject empty output textures.");
    }

    void testConstantsAndDispatch() {
        const lumin::render::gi::RtDiNrdFrameParameters parameters{
            .frameSlot = lumin::render::core::FrameSlotIndex{1},
            .extent = {65, 33},
            .cameraPosition = {2.0F, 3.0F, 4.0F},
            .jitterDeltaUv = {0.125F, -0.25F},
            .denoisingRange = 750.0F,
            .frameSlotFenceWaited = true,
        };
        const auto constants = lumin::render::gi::detail::makeRtDiNrdInputsConstants(parameters, 7);
        require(constants.cameraPosition == glm::vec4(2.0F, 3.0F, 4.0F, 1.0F) &&
                    constants.renderParameters == glm::vec4(0.125F, -0.25F, 750.0F, 0.0F) &&
                    constants.renderInfo == glm::uvec4(65U, 33U, 7U, 0xffffffffU),
                "RTDI preparation constants must match the Slang ABI.");

        const auto dispatch = lumin::render::gi::detail::makeRtDiNrdDispatchSize({65, 33});
        require(dispatch == lumin::render::gi::RtDiNrdDispatchSize{9, 5, 1},
                "RTDI preparation must cover odd extents with complete 8x8 groups.");
        requireThrows<std::invalid_argument>(
            [] {
                (void)lumin::render::gi::detail::makeRtDiNrdDispatchSize({0, 33});
            },
            "RTDI preparation must reject an empty dispatch.");

        auto invalid = parameters;
        invalid.denoisingRange = std::numeric_limits<float>::infinity();
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::gi::detail::makeRtDiNrdInputsConstants(invalid, 7);
            },
            "RTDI preparation must reject a non-finite denoising range.");
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
        lumin::render::gi::RtDiNrdDispatchSize dispatched;
    };

    void testBindingAbiAndValidation() {
        const nvrhi::BindingLayoutDesc layout = lumin::render::gi::detail::makeRtDiNrdInputsBindingLayoutDesc();
        require(layout.visibility == nvrhi::ShaderType::Compute && layout.registerSpace == 0 &&
                    layout.registerSpaceIsDescriptorSet && layout.bindings.size() == 15,
                "RTDI preparation must expose one explicit 15-binding compute descriptor set.");
        for (std::uint32_t binding = 0; binding < 8; ++binding) {
            require(layout.bindings[binding] == nvrhi::BindingLayoutItem::Texture_SRV(binding),
                    "RTDI preparation bindings 0 through 7 must be texture SRVs.");
        }
        require(layout.bindings[8] == nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8) &&
                    layout.bindings[9] == nvrhi::BindingLayoutItem::ConstantBuffer(9),
                "RTDI preparation material and constants bindings differ from the Slang ABI.");
        for (std::uint32_t binding = 10; binding < 15; ++binding) {
            require(layout.bindings[binding] == nvrhi::BindingLayoutItem::Texture_UAV(binding),
                    "RTDI preparation bindings 10 through 14 must be output UAVs.");
        }

        RtDiNrdInputResources inputs = makeInputs();
        const RtDiNrdSignalResources outputs = makeOutputs();
        nvrhi::BufferDesc constantsDesc;
        constantsDesc.byteSize = sizeof(lumin::render::gi::RtDiNrdInputsConstants);
        constantsDesc.isConstantBuffer = true;
        const nvrhi::BufferHandle constants = nvrhi::BufferHandle::Create(new FakeBuffer(constantsDesc));
        require(lumin::render::gi::detail::validateRtDiNrdInputResources(inputs, {65, 33}) == 3,
                "RTDI preparation must derive the exact GpuMaterialData record count.");

        const nvrhi::BindingSetDesc bindings =
            lumin::render::gi::detail::makeRtDiNrdInputsBindingSetDesc(inputs, outputs, constants);
        require(bindings.bindings.size() == 15 && bindings.bindings[0].resourceHandle == inputs.diffuseRadianceHitT &&
                    bindings.bindings[8].resourceHandle == inputs.materials &&
                    bindings.bindings[9].resourceHandle == constants &&
                    bindings.bindings[10].resourceHandle == outputs.diffuseRadianceHitDistance &&
                    bindings.bindings[14].resourceHandle == outputs.motion,
                "RTDI preparation binding set must retain every exact input and output resource.");

        inputs.viewZ = makeTexture(nvrhi::Format::RGBA16_FLOAT);
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::gi::detail::validateRtDiNrdInputResources(inputs, {65, 33});
            },
            "RTDI preparation must reject a view-Z texture with the wrong format.");

        DispatchProbe probe;
        nvrhi::ComputeState state;
        lumin::render::gi::detail::recordRtDiNrdDispatch(probe, state, {9, 5, 1});
        require(probe.stateSet == &state && probe.dispatched == lumin::render::gi::RtDiNrdDispatchSize{9, 5, 1},
                "RTDI preparation must bind compute state before dispatch.");
    }

    void testShaderBuildsReblurSignals() {
        const std::filesystem::path shaderPath =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "shaders/RtDiNrdInputs.slang";
        std::ifstream shader(shaderPath, std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>(shader), std::istreambuf_iterator<char>()};
        require(source.find("#include \"NRD.hlsli\"") != std::string::npos &&
                    source.find("NRD_MaterialFactors") != std::string::npos &&
                    source.find("REBLUR_FrontEnd_GetNormHitDist") != std::string::npos &&
                    source.find("REBLUR_FrontEnd_PackRadianceAndNormHitDist") != std::string::npos &&
                    source.find("rawDiffuse.rgb") != std::string::npos &&
                    source.find("rawSpecular.rgb") != std::string::npos &&
                    source.find("motionTexture.Load(int3(pixel, 0)) + frame.renderParameters.xy") !=
                        std::string::npos &&
                    source.find("denoiserViewZ[pixel] = frame.renderParameters.z") != std::string::npos,
                "RTDI preparation must demodulate both lobes and publish complete REBLUR auxiliary signals.");
    }

} // namespace

int main() {
    try {
        testSignalFormatsAndTextureContract();
        testConstantsAndDispatch();
        testBindingAbiAndValidation();
        testShaderBuildsReblurSignals();
        std::puts("RtDiNrdInputs PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RtDiNrdInputs FAIL: %s\n", error.what());
        return 1;
    }
}

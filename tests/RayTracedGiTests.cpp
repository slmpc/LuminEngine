#include "render/gi/RayTracedGi.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Callable> void requireInvalidArgument(Callable&& callable, const char* message) {
        try {
            callable();
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error(message);
    }

    void testNrdSignalFormatsAndTextureUsage() {
        const lumin::render::gi::RayTracedGiSignalFormats formats;
        require(formats.diffuseRadianceHitDistance == nvrhi::Format::RGBA16_FLOAT &&
                    formats.specularRadianceHitDistance == nvrhi::Format::RGBA16_FLOAT &&
                    formats.viewZ == nvrhi::Format::R32_FLOAT &&
                    formats.normalRoughness == nvrhi::Format::R10G10B10A2_UNORM &&
                    formats.motion == nvrhi::Format::RG16_FLOAT,
                "Raw RT GI signals must preserve NRD-compatible formats.");

        const nvrhi::TextureDesc desc = lumin::render::gi::detail::makeRayTracedGiSignalTextureDesc(
            1920, 1080, formats.diffuseRadianceHitDistance, "Diffuse hit distance");
        require(desc.width == 1920 && desc.height == 1080 && desc.format == nvrhi::Format::RGBA16_FLOAT &&
                    desc.isShaderResource && desc.isUAV && !desc.keepInitialState &&
                    desc.initialState == nvrhi::ResourceStates::Common,
                "RT GI signals must support FrameGraph-owned SRV/UAV transitions.");
        requireInvalidArgument(
            [] {
                (void)lumin::render::gi::detail::makeRayTracedGiSignalTextureDesc(0, 1080, nvrhi::Format::RGBA16_FLOAT,
                                                                                  "invalid");
            },
            "Zero-width GI signals must be rejected.");
    }

    void testBindingLayoutMatchesShaderAbi() {
        constexpr std::uint32_t geometryCapacity = 37;
        constexpr std::uint32_t materialTextureCapacity = 5;
        const nvrhi::BindingLayoutDesc desc = lumin::render::gi::detail::makeRayTracedGiBindingLayoutDesc(
            geometryCapacity, false, materialTextureCapacity);
        require(desc.visibility == nvrhi::ShaderType::AllRayTracing && desc.registerSpace == 0 &&
                    desc.registerSpaceIsDescriptorSet && desc.bindings.size() == 18,
                "RT GI set 0 must contain scene, material textures, and raw-signal resources.");
        require(desc.bindings[0].slot == 0 && desc.bindings[0].type == nvrhi::ResourceType::RayTracingAccelStruct,
                "Binding 0 must contain the TLAS.");
        require(desc.bindings[5].slot == 5 && desc.bindings[5].type == nvrhi::ResourceType::StructuredBuffer_SRV &&
                    desc.bindings[5].getArraySize() == geometryCapacity && desc.bindings[6].slot == 6 &&
                    desc.bindings[6].getArraySize() == geometryCapacity,
                "Bindings 5 and 6 must be equally sized bindless vertex/index arrays.");
        for (std::size_t index = 9; index <= 13; ++index) {
            require(desc.bindings[index].slot == index && desc.bindings[index].type == nvrhi::ResourceType::Texture_UAV,
                    "Bindings 9 through 13 must be the five raw GI UAV signals.");
        }
        require(desc.bindings[14].slot == 14 && desc.bindings[14].type == nvrhi::ResourceType::ConstantBuffer,
                "Binding 14 must contain RayTracedGiConstants.");
        require(desc.bindings[15].slot == 21 && desc.bindings[15].getArraySize() == materialTextureCapacity &&
                    desc.bindings[16].slot == 22 && desc.bindings[16].getArraySize() == materialTextureCapacity &&
                    desc.bindings[17].slot == 23 && desc.bindings[17].type == nvrhi::ResourceType::Sampler,
                "RT GI bindings 21-23 must expose both material texture arrays and their sampler.");

        const nvrhi::BindingLayoutDesc sharcDesc = lumin::render::gi::detail::makeRayTracedGiBindingLayoutDesc(
            geometryCapacity, true, materialTextureCapacity);
        require(sharcDesc.bindings.size() == 24,
                "SHARC query must contain cache bindings and the shared material texture table.");
        for (std::size_t index = 15; index <= 19; ++index) {
            require(sharcDesc.bindings[index].type == nvrhi::ResourceType::StructuredBuffer_UAV,
                    "SHARC query cache/statistics descriptors must use storage buffers.");
        }
        require(sharcDesc.bindings[20].type == nvrhi::ResourceType::ConstantBuffer,
                "SHARC query binding 20 must contain SharcGpuConstants.");
        require(sharcDesc.bindings[21].slot == 21 && sharcDesc.bindings[22].slot == 22 &&
                    sharcDesc.bindings[23].slot == 23,
                "SHARC RT GI must preserve material bindings 21-23 after its cache descriptors.");

        requireInvalidArgument(
            [] {
                (void)lumin::render::gi::detail::makeRayTracedGiBindingLayoutDesc(0);
            },
            "A zero-sized bindless geometry table must be rejected.");
        requireInvalidArgument(
            [] {
                (void)lumin::render::gi::detail::makeRayTracedGiBindingLayoutDesc(65536);
            },
            "Geometry descriptor capacity must fit the NvRHI ABI.");
    }

    class DispatchProbe {
    public:
        void setRayTracingState(const nvrhi::rt::State& state) {
            stateSet = &state;
        }

        void dispatchRays(const nvrhi::rt::DispatchRaysArguments& arguments) {
            dispatch = arguments;
        }

        const nvrhi::rt::State* stateSet = nullptr;
        nvrhi::rt::DispatchRaysArguments dispatch{};
    };

    void testDispatchUsesFullRenderExtent() {
        DispatchProbe probe;
        nvrhi::rt::State state;
        lumin::render::gi::detail::recordRayTracedGiDispatch(probe, state, 1280, 720);
        require(probe.stateSet == &state && probe.dispatch.width == 1280 && probe.dispatch.height == 720 &&
                    probe.dispatch.depth == 1,
                "RT GI must bind state before dispatching one ray-generation thread per pixel.");
    }

} // namespace

int main() {
    try {
        testNrdSignalFormatsAndTextureUsage();
        testBindingLayoutMatchesShaderAbi();
        testDispatchUsesFullRenderExtent();
        std::puts("RayTracedGi PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RayTracedGi FAIL: %s\n", error.what());
        return 1;
    }
}

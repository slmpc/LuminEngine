#include "render/gi/raytracing/RayTracedDirectLighting.hpp"
#include "render/gi/raytracing/RayTracedGi.hpp"
#include "scene/Environment.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    void requireNear(float actual, float expected, const char* message) {
        if (std::abs(actual - expected) > 1.0e-5F) {
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

    void testRayTracingSunIrradianceUsesPhysicalPreExposure() {
        lumin::scene::DirectionalLight sun;
        const glm::vec4 defaultIrradiance = lumin::render::gi::makeRayTracingSunIrradiance(sun, true);
        const float expected = sun.illuminanceLux * lumin::render::gi::physicalLightingPreExposure;
        requireNear(defaultIrradiance.x, sun.color.x * expected,
                    "RT sun irradiance must use EV100 physical pre-exposure instead of a Raster reference value.");
        requireNear(defaultIrradiance.y, sun.color.y * expected,
                    "RT sun irradiance must preserve the scene light tint.");
        requireNear(defaultIrradiance.z, sun.color.z * expected,
                    "RT sun irradiance must preserve the scene light tint.");
        requireNear(defaultIrradiance.w, 1.0F, "RT environment visibility must remain enabled.");

        sun.illuminanceLux *= 0.5F;
        const glm::vec4 halfIrradiance = lumin::render::gi::makeRayTracingSunIrradiance(sun, true);
        requireNear(halfIrradiance.x, defaultIrradiance.x * 0.5F,
                    "RT sun irradiance must respond linearly to scene illuminance.");

        const glm::vec4 disabledIrradiance = lumin::render::gi::makeRayTracingSunIrradiance(sun, false);
        require(disabledIrradiance.x == 0.0F && disabledIrradiance.y == 0.0F && disabledIrradiance.z == 0.0F &&
                    disabledIrradiance.w == 1.0F,
                "Disabling direct lighting must preserve only RT environment visibility.");
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
                    desc.registerSpaceIsDescriptorSet && desc.bindings.size() == 19,
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
        require(desc.bindings[18].slot == 24 && desc.bindings[18].type == nvrhi::ResourceType::Texture_SRV,
                "RT GI binding 24 must expose primary material IDs for Cook-Torrance evaluation.");

        const nvrhi::BindingLayoutDesc sharcDesc = lumin::render::gi::detail::makeRayTracedGiBindingLayoutDesc(
            geometryCapacity, true, materialTextureCapacity);
        require(sharcDesc.bindings.size() == 25,
                "SHARC query must contain cache bindings and the shared material texture table.");
        for (std::size_t index = 15; index <= 19; ++index) {
            require(sharcDesc.bindings[index].type == nvrhi::ResourceType::StructuredBuffer_UAV,
                    "SHARC query cache/statistics descriptors must use storage buffers.");
        }
        require(sharcDesc.bindings[20].type == nvrhi::ResourceType::ConstantBuffer,
                "SHARC query binding 20 must contain SharcGpuConstants.");
        require(sharcDesc.bindings[21].slot == 21 && sharcDesc.bindings[22].slot == 22 &&
                    sharcDesc.bindings[23].slot == 23 && sharcDesc.bindings[24].slot == 24,
                "SHARC RT GI must preserve material bindings 21-24 after its cache descriptors.");

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

    void testRayTracingShadersUseCookTorranceAndGgxSampling() {
        const std::filesystem::path shaderRoot =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "shaders";
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        };
        const std::string physicalLighting = read(shaderRoot / "include/PhysicalLighting.slang");
        const std::string rtDirect = read(shaderRoot / "RtDi.slang");
        const std::string rtGi = read(shaderRoot / "RtGi.slang");
        const std::string sharc = read(shaderRoot / "SharcUpdate.slang");

        require(physicalLighting.find("luminEvaluateCookTorrance") != std::string::npos &&
                    physicalLighting.find("luminGgxDistribution") != std::string::npos &&
                    physicalLighting.find("luminSmithGgxG1") != std::string::npos &&
                    physicalLighting.find("luminFresnelSchlick") != std::string::npos &&
                    physicalLighting.find("luminSampleCookTorranceSpecular") != std::string::npos &&
                    physicalLighting.find("luminClampFp16Radiance") != std::string::npos,
                "Shared physical lighting must implement Cook-Torrance with GGX, Smith, Schlick, and GGX sampling.");
        for (const std::string* source : {&rtDirect, &rtGi, &sharc}) {
            require(source->find("luminEvaluateCookTorrance") != std::string::npos,
                    "RTDI, RTGI, and SHARC must evaluate the shared Cook-Torrance BRDF.");
        }
        require(rtGi.find("diffuseSample.throughput") != std::string::npos &&
                    rtGi.find("specularSample.throughput") != std::string::npos &&
                    rtGi.find("0.015") == std::string::npos,
                "RTGI must apply BRDF/PDF throughput without a fixed ambient contribution.");
    }

} // namespace

int main() {
    try {
        testRayTracingSunIrradianceUsesPhysicalPreExposure();
        testNrdSignalFormatsAndTextureUsage();
        testBindingLayoutMatchesShaderAbi();
        testDispatchUsesFullRenderExtent();
        testRayTracingShadersUseCookTorranceAndGgxSampling();
        std::puts("RayTracedGi PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RayTracedGi FAIL: %s\n", error.what());
        return 1;
    }
}

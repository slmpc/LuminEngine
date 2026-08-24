#include "render/gi/raytracing/RayTracedDirectLighting.hpp"
#include "render/gi/raytracing/SharcIndirectLighting.hpp"
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
        const glm::vec4 irradiance = lumin::render::gi::makeRayTracingSunIrradiance(sun, true);
        const float expected = sun.illuminanceLux * lumin::render::gi::physicalLightingPreExposure;
        requireNear(irradiance.x, sun.color.x * expected, "RT sun irradiance must use physical pre-exposure.");
        requireNear(irradiance.y, sun.color.y * expected, "RT sun irradiance must preserve tint.");
        requireNear(irradiance.z, sun.color.z * expected, "RT sun irradiance must preserve tint.");
        requireNear(irradiance.w, 1.0F, "RT environment visibility must remain enabled.");
    }

    void testNrdSignalFormatsAndTextureUsage() {
        const lumin::render::gi::SharcIndirectLightingSignalFormats formats;
        require(formats.diffuseRadianceHitDistance == nvrhi::Format::RGBA16_FLOAT &&
                    formats.specularRadianceHitDistance == nvrhi::Format::RGBA16_FLOAT &&
                    formats.viewZ == nvrhi::Format::R32_FLOAT &&
                    formats.normalRoughness == nvrhi::Format::R10G10B10A2_UNORM &&
                    formats.motion == nvrhi::Format::RG16_FLOAT,
                "SHARC indirect signals must preserve NRD-compatible formats.");

        const nvrhi::TextureDesc desc = lumin::render::gi::detail::makeSharcIndirectLightingTextureDesc(
            1920, 1080, formats.diffuseRadianceHitDistance, "Diffuse hit distance");
        require(desc.width == 1920 && desc.height == 1080 && desc.format == nvrhi::Format::RGBA16_FLOAT &&
                    desc.isShaderResource && desc.isUAV && !desc.keepInitialState &&
                    desc.initialState == nvrhi::ResourceStates::Common,
                "SHARC indirect signals must support FrameGraph-owned SRV/UAV transitions.");
        requireInvalidArgument(
            [] {
                (void)lumin::render::gi::detail::makeSharcIndirectLightingTextureDesc(
                    0, 1080, nvrhi::Format::RGBA16_FLOAT, "invalid");
            },
            "Zero-width SHARC indirect signals must be rejected.");
    }

    void testBindingLayoutMatchesShaderAbi() {
        constexpr std::uint32_t geometryCapacity = 37;
        constexpr std::uint32_t materialTextureCapacity = 5;
        const nvrhi::BindingLayoutDesc desc = lumin::render::gi::detail::makeSharcIndirectLightingBindingLayoutDesc(
            geometryCapacity, materialTextureCapacity);
        require(desc.visibility == nvrhi::ShaderType::AllRayTracing && desc.registerSpace == 0 &&
                    desc.registerSpaceIsDescriptorSet && desc.bindings.size() == 26,
                "SHARC indirect set 0 must expose the scene, five NRD signals, SHARC, materials, and lights.");
        require(desc.bindings[0].slot == 0 && desc.bindings[0].type == nvrhi::ResourceType::RayTracingAccelStruct,
                "Binding 0 must contain the TLAS.");
        require(desc.bindings[6].slot == 6 && desc.bindings[6].getArraySize() == geometryCapacity &&
                    desc.bindings[7].slot == 7 && desc.bindings[7].getArraySize() == geometryCapacity,
                "Bindings 6 and 7 must be equally sized bindless vertex/index arrays.");
        for (std::size_t index = 10; index <= 14; ++index) {
            require(desc.bindings[index].slot == index && desc.bindings[index].type == nvrhi::ResourceType::Texture_UAV,
                    "Bindings 10 through 14 must be the five NRD UAV signals.");
        }
        require(desc.bindings[15].slot == 15 && desc.bindings[15].type == nvrhi::ResourceType::ConstantBuffer,
                "Binding 15 must contain SharcIndirectLightingConstants.");
        require(desc.bindings[21].slot == 21 && desc.bindings[21].type == nvrhi::ResourceType::ConstantBuffer,
                "Binding 21 must contain SharcGpuConstants.");
        require(desc.bindings[22].getArraySize() == materialTextureCapacity &&
                    desc.bindings[23].getArraySize() == materialTextureCapacity &&
                    desc.bindings[24].type == nvrhi::ResourceType::Sampler &&
                    desc.bindings[25].type == nvrhi::ResourceType::StructuredBuffer_SRV,
                "Bindings 22 through 25 must expose material textures, sampler, and the light table.");
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
        lumin::render::gi::detail::recordSharcIndirectLightingDispatch(probe, state, 1280, 720);
        require(probe.stateSet == &state && probe.dispatch.width == 1280 && probe.dispatch.height == 720 &&
                    probe.dispatch.depth == 1,
                "SHARC indirect must dispatch one ray-generation thread per pixel.");
    }

    void testShaderUsesOneLobeAndNrdSignals() {
        const std::filesystem::path shaderRoot =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "shaders";
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        };
        const std::string source = read(shaderRoot / "SharcIndirectLighting.slang");
        require(source.find("diffuseSelectionProbability") != std::string::npos &&
                    source.find("sample.throughput / selectionProbability") != std::string::npos &&
                    source.find("NRD_MaterialFactors") != std::string::npos &&
                    source.find("REBLUR_FrontEnd_PackRadianceAndNormHitDist") != std::string::npos &&
                    source.find("SharcGetCachedRadiance") != std::string::npos &&
                    source.find("luminSampleLightIndex") != std::string::npos,
                "SHARC indirect must sample one unbiased lobe, query cache, and publish NRD signals.");
    }

} // namespace

int main() {
    try {
        testRayTracingSunIrradianceUsesPhysicalPreExposure();
        testNrdSignalFormatsAndTextureUsage();
        testBindingLayoutMatchesShaderAbi();
        testDispatchUsesFullRenderExtent();
        testShaderUsesOneLobeAndNrdSignals();
        std::puts("SharcIndirectLighting PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SharcIndirectLighting FAIL: %s\n", error.what());
        return 1;
    }
}

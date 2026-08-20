#include "render/resources/PipelineFactory.hpp"
#include "render/resources/ShaderLibrary.hpp"

#include <nvrhi/nvrhi.h>

#include <array>
#include <concepts>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

    static_assert(requires(const lumin::render::PipelineFactory& factory,
                           const lumin::render::ComputePipelineDesc& computeDesc,
                           const lumin::render::RayTracingPipelineDesc& rayTracingDesc,
                           const nvrhi::rt::PipelineHandle& pipeline,
                           const lumin::render::RayTracingShaderTableDesc& tableDesc) {
        { factory.createComputePipeline(computeDesc) } -> std::same_as<nvrhi::ComputePipelineHandle>;
        { factory.createRayTracingPipeline(rayTracingDesc) } -> std::same_as<nvrhi::rt::PipelineHandle>;
        { factory.createRayTracingShaderTable(pipeline, tableDesc) } -> std::same_as<nvrhi::rt::ShaderTableHandle>;
    });

    static_assert(requires(const lumin::render::ShaderLibrary& library, const std::filesystem::path& path,
                           const lumin::render::ShaderModuleDesc& desc) {
        { library.loadModule(desc) } -> std::same_as<nvrhi::ShaderHandle>;
        { library.loadComputeModule(path, std::string_view{"computeMain"}) } -> std::same_as<nvrhi::ShaderHandle>;
        {
            library.loadRayTracingModule(path, nvrhi::ShaderType::RayGeneration, std::string_view{"rayGenerationMain"})
        } -> std::same_as<nvrhi::ShaderHandle>;
    });

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Callable> void requireInvalidArgument(Callable&& callable, const char* message) {
        try {
            std::forward<Callable>(callable)();
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error(message);
    }

    class FakeShader final : public nvrhi::IShader {
    public:
        explicit FakeShader(nvrhi::ShaderType type) {
            desc_.setShaderType(type).setEntryName("testMain").setDebugName("Test shader");
        }

        unsigned long AddRef() override {
            return ++referenceCount_;
        }

        unsigned long Release() override {
            const unsigned long remaining = --referenceCount_;
            if (remaining == 0) {
                delete this;
            }
            return remaining;
        }

        unsigned long GetRefCount() override {
            return referenceCount_;
        }

        [[nodiscard]] const nvrhi::ShaderDesc& getDesc() const override {
            return desc_;
        }

        void getBytecode(const void** bytecode, std::size_t* size) const override {
            *bytecode = nullptr;
            *size = 0;
        }

    private:
        unsigned long referenceCount_ = 0;
        nvrhi::ShaderDesc desc_;
    };

    [[nodiscard]] nvrhi::ShaderHandle makeShader(nvrhi::ShaderType type) {
        return nvrhi::ShaderHandle(new FakeShader(type));
    }

    void testShaderModuleDescription() {
        lumin::render::ShaderModuleDesc desc;
        desc.fileName = "gi.comp.spv";
        desc.shaderType = nvrhi::ShaderType::Compute;
        desc.entryPoint = "computeMain";
        desc.debugName = "GI dispatch";
        desc.hlslExtensionsUAV = 7;

        const nvrhi::ShaderDesc nativeDesc = lumin::render::detail::makeShaderDesc(desc);
        require(nativeDesc.shaderType == nvrhi::ShaderType::Compute, "Shader stage was not preserved.");
        require(nativeDesc.entryName == "computeMain", "Shader entry point was not preserved.");
        require(nativeDesc.debugName == "GI dispatch", "Shader debug name was not preserved.");
        require(nativeDesc.hlslExtensionsUAV == 7, "Shader extension UAV was not preserved.");
        require(lumin::render::detail::isRayTracingShaderType(nvrhi::ShaderType::RayGeneration),
                "Ray-generation must be recognized as an RT shader stage.");
        require(!lumin::render::detail::isRayTracingShaderType(nvrhi::ShaderType::Compute),
                "Compute must not be recognized as an RT shader stage.");
        require(!lumin::render::detail::isRayTracingShaderType(nvrhi::ShaderType::AllRayTracing),
                "An RT stage mask must not be accepted as a single shader stage.");

        desc.entryPoint.clear();
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeShaderDesc(desc);
            },
            "Empty shader entry points must be rejected.");
        desc.entryPoint = "computeMain";
        desc.shaderType = nvrhi::ShaderType::AllRayTracing;
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeShaderDesc(desc);
            },
            "Shader stage masks must be rejected.");
    }

    void testComputePipelineDescription() {
        lumin::render::ComputePipelineDesc desc;
        desc.computeShader = makeShader(nvrhi::ShaderType::Compute);
        const nvrhi::ComputePipelineDesc nativeDesc = lumin::render::detail::makeComputePipelineDesc(desc);
        require(nativeDesc.CS == desc.computeShader.Get(), "Compute shader was not preserved.");
        require(nativeDesc.bindingLayouts.empty(), "Unexpected compute binding layouts were emitted.");

        desc.computeShader = makeShader(nvrhi::ShaderType::Pixel);
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeComputePipelineDesc(desc);
            },
            "A non-compute shader must be rejected by the compute pipeline contract.");

        desc.computeShader = makeShader(nvrhi::ShaderType::Compute);
        const std::array<nvrhi::BindingLayoutHandle, nvrhi::c_MaxBindingLayouts + 1> tooManyLayouts{};
        desc.bindingLayouts = tooManyLayouts;
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeComputePipelineDesc(desc);
            },
            "Compute binding layout overflow must be rejected before device creation.");
    }

    struct RayTracingFixture {
        nvrhi::ShaderHandle rayGeneration = makeShader(nvrhi::ShaderType::RayGeneration);
        nvrhi::ShaderHandle miss = makeShader(nvrhi::ShaderType::Miss);
        nvrhi::ShaderHandle callable = makeShader(nvrhi::ShaderType::Callable);
        nvrhi::ShaderHandle closestHit = makeShader(nvrhi::ShaderType::ClosestHit);
        nvrhi::ShaderHandle intersection = makeShader(nvrhi::ShaderType::Intersection);

        std::array<lumin::render::RayTracingPipelineShaderDesc, 3> shaders = {
            lumin::render::RayTracingPipelineShaderDesc{"RayGen", rayGeneration},
            lumin::render::RayTracingPipelineShaderDesc{"Miss", miss},
            lumin::render::RayTracingPipelineShaderDesc{"Callable", callable},
        };
        std::array<lumin::render::RayTracingHitGroupDesc, 2> hitGroups = {
            lumin::render::RayTracingHitGroupDesc{"TriangleHit", closestHit, nullptr, nullptr, false},
            lumin::render::RayTracingHitGroupDesc{"ProceduralHit", closestHit, nullptr, intersection, true},
        };

        [[nodiscard]] lumin::render::RayTracingPipelineDesc desc() const {
            lumin::render::RayTracingPipelineDesc result;
            result.shaders = shaders;
            result.hitGroups = hitGroups;
            result.maxPayloadSize = 32;
            result.maxAttributeSize = 8;
            result.maxRecursionDepth = 2;
            return result;
        }
    };

    void testRayTracingPipelineDescription() {
        RayTracingFixture fixture;
        const nvrhi::rt::PipelineDesc nativeDesc = lumin::render::detail::makeRayTracingPipelineDesc(fixture.desc());
        require(nativeDesc.shaders.size() == 3 && nativeDesc.hitGroups.size() == 2,
                "RT exports were not fully translated.");
        require(nativeDesc.shaders.front().exportName == "RayGen", "RT shader export name was not preserved.");
        require(nativeDesc.hitGroups.back().isProceduralPrimitive, "Procedural hit-group metadata was not preserved.");
        require(nativeDesc.maxPayloadSize == 32 && nativeDesc.maxRecursionDepth == 2,
                "RT pipeline interface limits were not preserved.");

        auto invalidDesc = fixture.desc();
        invalidDesc.maxRecursionDepth = 0;
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeRayTracingPipelineDesc(invalidDesc);
            },
            "Zero RT recursion depth must be rejected.");

        fixture.hitGroups[1].proceduralPrimitive = false;
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeRayTracingPipelineDesc(fixture.desc());
            },
            "Triangle hit groups must reject intersection shaders.");
        fixture.hitGroups[1].proceduralPrimitive = true;

        fixture.shaders[1].exportName = "RayGen";
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeRayTracingPipelineDesc(fixture.desc());
            },
            "Duplicate RT export names must be rejected.");
        fixture.shaders[1].exportName = "Miss";

        fixture.shaders[0].shader = makeShader(nvrhi::ShaderType::Compute);
        requireInvalidArgument(
            [&] {
                (void)lumin::render::detail::makeRayTracingPipelineDesc(fixture.desc());
            },
            "Compute shaders must not appear as general RT exports.");
    }

    void testRayTracingShaderTableDescription() {
        RayTracingFixture fixture;
        const nvrhi::rt::PipelineDesc pipelineDesc = lumin::render::detail::makeRayTracingPipelineDesc(fixture.desc());
        const std::array<lumin::render::RayTracingShaderTableEntryDesc, 2> missEntries = {
            lumin::render::RayTracingShaderTableEntryDesc{"Miss"},
            lumin::render::RayTracingShaderTableEntryDesc{"Miss"},
        };
        const std::array<lumin::render::RayTracingShaderTableEntryDesc, 1> hitEntries = {
            lumin::render::RayTracingShaderTableEntryDesc{"TriangleHit"},
        };
        const std::array<lumin::render::RayTracingShaderTableEntryDesc, 1> callableEntries = {
            lumin::render::RayTracingShaderTableEntryDesc{"Callable"},
        };

        lumin::render::RayTracingShaderTableDesc tableDesc;
        tableDesc.rayGenerationExport = "RayGen";
        tableDesc.missShaders = missEntries;
        tableDesc.hitGroups = hitEntries;
        tableDesc.callableShaders = callableEntries;
        tableDesc.cached = true;
        tableDesc.maxEntries = 5;
        tableDesc.debugName = "GI shader table";

        lumin::render::detail::validateRayTracingShaderTable(pipelineDesc, tableDesc);
        const nvrhi::rt::ShaderTableDesc nativeDesc = lumin::render::detail::makeRayTracingShaderTableDesc(tableDesc);
        require(nativeDesc.isCached && nativeDesc.maxEntries == 5 && nativeDesc.debugName == "GI shader table",
                "Cached shader table configuration was not preserved.");

        tableDesc.maxEntries = 4;
        requireInvalidArgument(
            [&] {
                lumin::render::detail::validateRayTracingShaderTable(pipelineDesc, tableDesc);
            },
            "Cached shader table capacity must include the ray-generation record.");
        tableDesc.maxEntries = 5;

        tableDesc.rayGenerationExport = "Miss";
        requireInvalidArgument(
            [&] {
                lumin::render::detail::validateRayTracingShaderTable(pipelineDesc, tableDesc);
            },
            "A miss export must not be accepted as the ray-generation record.");
        tableDesc.rayGenerationExport = "RayGen";

        auto missingHitEntries = hitEntries;
        missingHitEntries.front().exportName = "MissingHit";
        tableDesc.hitGroups = missingHitEntries;
        requireInvalidArgument(
            [&] {
                lumin::render::detail::validateRayTracingShaderTable(pipelineDesc, tableDesc);
            },
            "Unknown hit-group exports must be rejected.");
    }

} // namespace

int main() {
    try {
        testShaderModuleDescription();
        testComputePipelineDescription();
        testRayTracingPipelineDescription();
        testRayTracingShaderTableDescription();
        std::puts("RenderPipelineContracts PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RenderPipelineContracts FAIL: %s\n", error.what());
        return 1;
    }
}

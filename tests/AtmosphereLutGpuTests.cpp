#include "render/atmosphere/AtmosphereLutGpu.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {

    using lumin::render::atmosphere::AtmosphereLut;
    using lumin::render::atmosphere::AtmosphereLutNativeResources;
    using lumin::render::atmosphere::AtmosphereLutResourceDesc;

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

    class FakeSampler final : public nvrhi::RefCounter<nvrhi::ISampler> {
    public:
        explicit FakeSampler(nvrhi::SamplerDesc desc) : desc_(desc) {
        }

        [[nodiscard]] const nvrhi::SamplerDesc& getDesc() const override {
            return desc_;
        }

    private:
        nvrhi::SamplerDesc desc_;
    };

    void testTextureBufferAndSamplerDescriptions() {
        const auto resources = lumin::render::atmosphere::makeAtmosphereLutResourceSet();
        for (const AtmosphereLutResourceDesc& resource : resources) {
            const nvrhi::TextureDesc desc = lumin::render::atmosphere::detail::makeAtmosphereLutTextureDesc(resource);
            require(desc.width == resource.extent.width && desc.height == resource.extent.height &&
                        desc.depth == resource.extent.depth && desc.dimension == resource.dimension &&
                        desc.format == nvrhi::Format::RGBA16_FLOAT && desc.isShaderResource && desc.isUAV &&
                        desc.initialState == nvrhi::ResourceStates::Common && !desc.keepInitialState,
                    "Atmosphere LUT textures must preserve the persistent SRV/UAV resource contract.");
        }

        const nvrhi::BufferDesc constants = lumin::render::atmosphere::detail::makeAtmosphereLutConstantBufferDesc();
        require(constants.byteSize == sizeof(lumin::render::atmosphere::AtmosphereGpuConstants) &&
                    constants.isConstantBuffer && constants.cpuAccess == nvrhi::CpuAccessMode::Write &&
                    constants.initialState == nvrhi::ResourceStates::Common && !constants.keepInitialState,
                "Atmosphere constants must use one CPU-writable buffer per frame slot.");

        const nvrhi::SamplerDesc sampler = lumin::render::atmosphere::detail::makeAtmosphereLutSamplerDesc();
        require(sampler.minFilter && sampler.magFilter && sampler.mipFilter &&
                    sampler.addressU == nvrhi::SamplerAddressMode::ClampToEdge &&
                    sampler.addressV == nvrhi::SamplerAddressMode::ClampToEdge &&
                    sampler.addressW == nvrhi::SamplerAddressMode::ClampToEdge,
                "Atmosphere LUT sampling must use linear clamp in every dimension.");
    }

    void requireLayout(AtmosphereLut target, std::initializer_list<nvrhi::BindingLayoutItem> expected) {
        const nvrhi::BindingLayoutDesc desc =
            lumin::render::atmosphere::detail::makeAtmosphereLutBindingLayoutDesc(target);
        require(desc.visibility == nvrhi::ShaderType::Compute && desc.registerSpace == 0 &&
                    desc.registerSpaceIsDescriptorSet && desc.bindingOffsets.shaderResource == 0 &&
                    desc.bindingOffsets.sampler == 0 && desc.bindingOffsets.constantBuffer == 0 &&
                    desc.bindingOffsets.unorderedAccess == 0 && desc.bindings.size() == expected.size(),
                "Atmosphere LUT binding layout must use explicit Vulkan set 0 bindings without register offsets.");
        std::size_t index = 0;
        for (const nvrhi::BindingLayoutItem& item : expected) {
            require(desc.bindings[index] == item, "Atmosphere LUT binding layout differs from the Slang ABI.");
            ++index;
        }
    }

    void testBindingLayoutsMatchEachEntry() {
        requireLayout(AtmosphereLut::Transmittance,
                      {nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_UAV(4)});
        requireLayout(AtmosphereLut::MultiScattering,
                      {nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
                       nvrhi::BindingLayoutItem::Sampler(3), nvrhi::BindingLayoutItem::Texture_UAV(4)});
        for (const AtmosphereLut target : {AtmosphereLut::SkyView, AtmosphereLut::AerialPerspective}) {
            requireLayout(target, {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                                   nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2),
                                   nvrhi::BindingLayoutItem::Sampler(3), nvrhi::BindingLayoutItem::Texture_UAV(4)});
        }
    }

    AtmosphereLutNativeResources makeNativeResources() {
        AtmosphereLutNativeResources result;
        const auto descriptions = lumin::render::atmosphere::makeAtmosphereLutResourceSet();
        for (const AtmosphereLutResourceDesc& resource : descriptions) {
            result.textures[lumin::render::atmosphere::atmosphereLutResourceIndex(resource.lut)] =
                nvrhi::TextureHandle::Create(
                    new FakeTexture(lumin::render::atmosphere::detail::makeAtmosphereLutTextureDesc(resource)));
        }
        result.sampler = nvrhi::SamplerHandle::Create(
            new FakeSampler(lumin::render::atmosphere::detail::makeAtmosphereLutSamplerDesc()));
        return result;
    }

    void testBindingSetsUseExactResourcesAndDimensions() {
        AtmosphereLutNativeResources resources = makeNativeResources();
        const nvrhi::BufferHandle constants = nvrhi::BufferHandle::Create(
            new FakeBuffer(lumin::render::atmosphere::detail::makeAtmosphereLutConstantBufferDesc()));

        const nvrhi::BindingSetDesc sky = lumin::render::atmosphere::detail::makeAtmosphereLutBindingSetDesc(
            AtmosphereLut::SkyView, constants, resources);
        require(sky.bindings.size() == 5 && sky.bindings[0].slot == 0 &&
                    sky.bindings[0].type == nvrhi::ResourceType::ConstantBuffer && sky.bindings[1].slot == 1 &&
                    sky.bindings[1].resourceHandle == resources.texture(AtmosphereLut::Transmittance) &&
                    sky.bindings[2].slot == 2 &&
                    sky.bindings[2].resourceHandle == resources.texture(AtmosphereLut::MultiScattering) &&
                    sky.bindings[3].slot == 3 && sky.bindings[3].type == nvrhi::ResourceType::Sampler &&
                    sky.bindings[4].slot == 4 && sky.bindings[4].type == nvrhi::ResourceType::Texture_UAV &&
                    sky.bindings[4].dimension == nvrhi::TextureDimension::Texture2D,
                "Sky-view bindings must expose CB, both upstream LUTs, sampler, and a 2D UAV.");

        const nvrhi::BindingSetDesc aerial = lumin::render::atmosphere::detail::makeAtmosphereLutBindingSetDesc(
            AtmosphereLut::AerialPerspective, constants, resources);
        require(aerial.bindings.size() == 5 && aerial.bindings.back().slot == 4 &&
                    aerial.bindings.back().dimension == nvrhi::TextureDimension::Texture3D &&
                    aerial.bindings.back().resourceHandle == resources.texture(AtmosphereLut::AerialPerspective),
                "Aerial perspective must bind its output as a 3D UAV.");

        resources.textures[lumin::render::atmosphere::atmosphereLutResourceIndex(AtmosphereLut::AerialPerspective)] =
            resources.texture(AtmosphereLut::SkyView);
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::atmosphere::detail::makeAtmosphereLutBindingSetDesc(
                    AtmosphereLut::AerialPerspective, constants, resources);
            },
            "A 2D texture must not be accepted as the aerial-perspective UAV.");
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
        lumin::render::atmosphere::AtmosphereLutDispatchSize dispatched;
    };

    void testDispatchSizesAndRecording() {
        const auto resources = lumin::render::atmosphere::makeAtmosphereLutResourceSet();
        require(lumin::render::atmosphere::detail::makeAtmosphereLutDispatchSize(resources[0]) ==
                        lumin::render::atmosphere::AtmosphereLutDispatchSize{32, 8, 1} &&
                    lumin::render::atmosphere::detail::makeAtmosphereLutDispatchSize(resources[1]) ==
                        lumin::render::atmosphere::AtmosphereLutDispatchSize{4, 4, 1} &&
                    lumin::render::atmosphere::detail::makeAtmosphereLutDispatchSize(resources[2]) ==
                        lumin::render::atmosphere::AtmosphereLutDispatchSize{32, 32, 1} &&
                    lumin::render::atmosphere::detail::makeAtmosphereLutDispatchSize(resources[3]) ==
                        lumin::render::atmosphere::AtmosphereLutDispatchSize{8, 8, 8},
                "Default LUT dispatch sizes must match the reflected Slang thread groups.");

        AtmosphereLutResourceDesc odd = resources[3];
        odd.extent = {33, 17, 5};
        const auto dispatch = lumin::render::atmosphere::detail::makeAtmosphereLutDispatchSize(odd);
        require(dispatch == lumin::render::atmosphere::AtmosphereLutDispatchSize{9, 5, 2},
                "Custom LUT dimensions must round every dispatch dimension upward.");

        DispatchProbe probe;
        nvrhi::ComputeState state;
        lumin::render::atmosphere::detail::recordAtmosphereLutDispatch(probe, state, dispatch);
        require(probe.stateSet == &state && probe.dispatched == dispatch,
                "Atmosphere dispatch must bind compute state before dispatching the calculated groups.");
        requireThrows<std::invalid_argument>(
            [&] {
                lumin::render::atmosphere::detail::recordAtmosphereLutDispatch(
                    probe, state, lumin::render::atmosphere::AtmosphereLutDispatchSize{});
            },
            "A zero-sized atmosphere dispatch must be rejected.");
    }

} // namespace

int main() {
    try {
        testTextureBufferAndSamplerDescriptions();
        testBindingLayoutsMatchEachEntry();
        testBindingSetsUseExactResourcesAndDimensions();
        testDispatchSizesAndRecording();
        std::puts("AtmosphereLutGpu PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "AtmosphereLutGpu FAIL: %s\n", error.what());
        return 1;
    }
}

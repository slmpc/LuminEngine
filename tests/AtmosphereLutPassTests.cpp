#include "render/atmosphere/AtmosphereLutPassPlan.hpp"
#include "render/atmosphere/AtmosphereLutResources.hpp"
#include "render/atmosphere/AtmosphereLutScheduler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    using lumin::render::FrameGraph;
    using lumin::render::FrameGraphTextureDesc;
    using lumin::render::atmosphere::AtmosphereLut;
    using lumin::render::atmosphere::AtmosphereLutFrameGraphResources;
    using lumin::render::atmosphere::AtmosphereLutFrameInput;
    using lumin::render::atmosphere::AtmosphereLutPassAccess;
    using lumin::render::atmosphere::AtmosphereLutPassPlan;
    using lumin::render::atmosphere::AtmosphereLutPassResourceUse;
    using lumin::render::atmosphere::AtmosphereLutQuality;
    using lumin::render::atmosphere::AtmosphereLutScheduler;
    using lumin::render::atmosphere::AtmosphereLutSignatures;
    using lumin::render::atmosphere::LightingSignature;
    using lumin::render::atmosphere::OpticalSignature;
    using lumin::render::atmosphere::SurfaceSignature;
    using lumin::render::atmosphere::ViewSignature;
    using lumin::render::core::RenderSequence;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Exception, typename Function>
    void requireThrows(Function&& function, const std::string& message) {
        try {
            std::forward<Function>(function)();
        } catch (const Exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    [[nodiscard]] AtmosphereLutSignatures signatures(std::uint64_t optical = 1, std::uint64_t surface = 2,
                                                     std::uint64_t lighting = 3, std::uint64_t sky = 4,
                                                     std::uint64_t aerial = 5) {
        return AtmosphereLutSignatures{
            .optical = OpticalSignature{optical},
            .surface = SurfaceSignature{surface},
            .lighting = LightingSignature{lighting},
            .skyView = ViewSignature{sky},
            .aerialPerspective = ViewSignature{aerial},
        };
    }

    [[nodiscard]] std::vector<AtmosphereLut> targets(const AtmosphereLutPassPlan& plan) {
        std::vector<AtmosphereLut> result;
        for (const auto& pass : plan.passes()) {
            result.push_back(pass.target());
        }
        return result;
    }

    void requireUses(std::span<const AtmosphereLutPassResourceUse> actual,
                     std::initializer_list<AtmosphereLutPassResourceUse> expected, const std::string& message) {
        require(actual.size() == expected.size() &&
                    std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()),
                message);
    }

    void testDefaultResourceContractAndMemoryEstimate() {
        const auto resources = lumin::render::atmosphere::makeAtmosphereLutResourceSet();
        const auto& transmittance =
            lumin::render::atmosphere::atmosphereLutResource(resources, AtmosphereLut::Transmittance);
        const auto& multi = lumin::render::atmosphere::atmosphereLutResource(resources, AtmosphereLut::MultiScattering);
        const auto& sky = lumin::render::atmosphere::atmosphereLutResource(resources, AtmosphereLut::SkyView);
        const auto& aerial =
            lumin::render::atmosphere::atmosphereLutResource(resources, AtmosphereLut::AerialPerspective);

        require(transmittance.extent == lumin::render::atmosphere::AtmosphereLutExtent{256, 64, 1} &&
                    multi.extent == lumin::render::atmosphere::AtmosphereLutExtent{32, 32, 1} &&
                    sky.extent == lumin::render::atmosphere::AtmosphereLutExtent{256, 256, 1} &&
                    aerial.extent == lumin::render::atmosphere::AtmosphereLutExtent{32, 32, 32},
                "Default atmosphere LUT dimensions are unstable.");
        require(transmittance.dimension == nvrhi::TextureDimension::Texture2D &&
                    multi.dimension == nvrhi::TextureDimension::Texture2D &&
                    sky.dimension == nvrhi::TextureDimension::Texture2D &&
                    aerial.dimension == nvrhi::TextureDimension::Texture3D,
                "Only aerial perspective may use a 3D texture.");

        const nvrhi::FormatSupport required = lumin::render::atmosphere::requiredAtmosphereLutFormatSupport();
        const auto requiredBits = static_cast<std::uint32_t>(required);
        require(transmittance.format == nvrhi::Format::RGBA16_FLOAT && multi.format == nvrhi::Format::RGBA16_FLOAT &&
                    sky.format == nvrhi::Format::RGBA16_FLOAT && aerial.format == nvrhi::Format::RGBA16_FLOAT,
                "All default atmosphere LUTs must use RGBA16_FLOAT.");
        require((requiredBits & static_cast<std::uint32_t>(nvrhi::FormatSupport::Texture)) != 0 &&
                    (requiredBits & static_cast<std::uint32_t>(nvrhi::FormatSupport::ShaderSample)) != 0 &&
                    (requiredBits & static_cast<std::uint32_t>(nvrhi::FormatSupport::ShaderUavStore)) != 0,
                "Atmosphere LUT format support must cover texture sampling and UAV stores.");
        for (const auto& resource : resources) {
            require(lumin::render::atmosphere::validateAtmosphereLutResourceDesc(resource),
                    "Generated atmosphere LUT descriptions must satisfy their own contract.");
            require(lumin::render::atmosphere::supportsAtmosphereLutFormat(resource, required),
                    "The exact required format flags must be accepted.");
        }

        require(lumin::render::atmosphere::estimateAtmosphereLutPayloadBytes(transmittance) == 256ULL * 64ULL * 8ULL &&
                    lumin::render::atmosphere::estimateAtmosphereLutPayloadBytes(multi) == 32ULL * 32ULL * 8ULL &&
                    lumin::render::atmosphere::estimateAtmosphereLutPayloadBytes(sky) == 256ULL * 256ULL * 8ULL &&
                    lumin::render::atmosphere::estimateAtmosphereLutPayloadBytes(aerial) ==
                        32ULL * 32ULL * 32ULL * 8ULL &&
                    lumin::render::atmosphere::estimateAtmosphereLutPayloadBytes(resources) == 925696ULL,
                "RGBA16_FLOAT LUT payload estimates are incorrect.");

        const auto missingStoreBits = requiredBits & ~static_cast<std::uint32_t>(nvrhi::FormatSupport::ShaderUavStore);
        require(!lumin::render::atmosphere::supportsAtmosphereLutFormat(
                    transmittance, static_cast<nvrhi::FormatSupport>(missingStoreBits)),
                "A format without UAV store support must be rejected.");
    }

    void testInvalidQualityIsRejected() {
        AtmosphereLutQuality invalid;
        invalid.transmittance.width = 0;
        require(!lumin::render::atmosphere::validateAtmosphereLutQuality(invalid),
                "A zero LUT dimension must be rejected.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::atmosphere::makeAtmosphereLutResourceSet(invalid);
            },
            "Resource construction must reject zero dimensions.");

        invalid = {};
        invalid.skyView.depth = 2;
        require(!lumin::render::atmosphere::validateAtmosphereLutQuality(invalid),
                "A 2D sky-view LUT cannot have multiple depth slices.");

        invalid = {};
        invalid.aerialPerspective = {
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
        };
        require(!lumin::render::atmosphere::validateAtmosphereLutQuality(invalid),
                "A LUT payload overflowing uint64 must be rejected.");

        invalid = {};
        constexpr std::uint32_t nearUint64PayloadHeight = 1U << 29U;
        invalid.transmittance = {
            std::numeric_limits<std::uint32_t>::max(),
            nearUint64PayloadHeight,
            1,
        };
        invalid.multiScattering = invalid.transmittance;
        require(!lumin::render::atmosphere::validateAtmosphereLutQuality(invalid),
                "The combined payload must be checked even when every individual LUT fits uint64.");

        requireThrows<std::invalid_argument>(
            [] {
                (void)lumin::render::atmosphere::makeAtmosphereLutPassPlan({});
            },
            "An uninitialized scheduler plan must be rejected.");
    }

    void testStableTopologyAndExplicitResourceUses() {
        AtmosphereLutScheduler scheduler;
        const auto schedulerPlan = scheduler.beginFrame({RenderSequence{0}, signatures()});
        const auto plan = lumin::render::atmosphere::makeAtmosphereLutPassPlan(schedulerPlan);
        require(plan.isValid() && plan.sequence() == RenderSequence{0},
                "The pass plan must preserve the scheduler sequence.");
        require(targets(plan) == std::vector<AtmosphereLut>{AtmosphereLut::Transmittance,
                                                            AtmosphereLut::MultiScattering, AtmosphereLut::SkyView,
                                                            AtmosphereLut::AerialPerspective},
                "The full atmosphere topology must be stable.");

        const auto passes = plan.passes();
        requireUses(passes[0].resourceUses(), {{AtmosphereLut::Transmittance, AtmosphereLutPassAccess::StorageWrite}},
                    "Transmittance must only write its LUT.");
        requireUses(passes[1].resourceUses(),
                    {{AtmosphereLut::Transmittance, AtmosphereLutPassAccess::SampledRead},
                     {AtmosphereLut::MultiScattering, AtmosphereLutPassAccess::StorageWrite}},
                    "Multi-scattering must read transmittance and write itself.");
        requireUses(passes[2].resourceUses(),
                    {{AtmosphereLut::Transmittance, AtmosphereLutPassAccess::SampledRead},
                     {AtmosphereLut::MultiScattering, AtmosphereLutPassAccess::SampledRead},
                     {AtmosphereLut::SkyView, AtmosphereLutPassAccess::StorageWrite}},
                    "Sky-view LUT dependencies are incomplete.");
        requireUses(passes[3].resourceUses(),
                    {{AtmosphereLut::Transmittance, AtmosphereLutPassAccess::SampledRead},
                     {AtmosphereLut::MultiScattering, AtmosphereLutPassAccess::SampledRead},
                     {AtmosphereLut::AerialPerspective, AtmosphereLutPassAccess::StorageWrite}},
                    "Aerial-perspective LUT dependencies are incomplete.");
        require(
            passes[0].dependencies().empty() && passes[1].dependencies().size() == 1 &&
                passes[1].dependencies()[0] == AtmosphereLut::Transmittance && passes[2].dependencies().size() == 1 &&
                passes[2].dependencies()[0] == AtmosphereLut::MultiScattering && passes[3].dependencies().size() == 1 &&
                passes[3].dependencies()[0] == AtmosphereLut::MultiScattering,
            "The direct producer topology must be Transmittance -> MultiScattering -> both view LUTs.");
        require(AtmosphereLutPassPlan::synchronizationOwner ==
                    lumin::render::atmosphere::AtmosphereLutSynchronizationOwner::FrameGraph,
                "FrameGraph must be the sole atmosphere LUT synchronization owner.");

        for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(AtmosphereLut::Count); ++index) {
            require(scheduler.state(static_cast<AtmosphereLut>(index)).generation == 0,
                    "Building a CPU pass plan must not advance scheduler generation.");
        }
        scheduler.abandonFrame(RenderSequence{0});
    }

    void testMinimalPassSets() {
        AtmosphereLutScheduler scheduler;
        AtmosphereLutSignatures current = signatures();
        scheduler.commitSubmittedFrame(
            scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{0}, current}).sequence());

        auto schedulerPlan = scheduler.beginFrame({RenderSequence{1}, current});
        require(lumin::render::atmosphere::makeAtmosphereLutPassPlan(schedulerPlan).empty(),
                "Stable inputs must not create compute passes.");
        scheduler.commitSubmittedFrame(RenderSequence{1});

        current.aerialPerspective = ViewSignature{50};
        schedulerPlan = scheduler.beginFrame({RenderSequence{2}, current});
        require(targets(lumin::render::atmosphere::makeAtmosphereLutPassPlan(schedulerPlan)) ==
                    std::vector<AtmosphereLut>{AtmosphereLut::AerialPerspective},
                "A full-view change must create only the aerial-perspective pass.");
        scheduler.commitSubmittedFrame(RenderSequence{2});

        current.lighting = LightingSignature{30};
        schedulerPlan = scheduler.beginFrame({RenderSequence{3}, current});
        require(targets(lumin::render::atmosphere::makeAtmosphereLutPassPlan(schedulerPlan)) ==
                    std::vector<AtmosphereLut>{AtmosphereLut::SkyView, AtmosphereLut::AerialPerspective},
                "A lighting change must create only both view passes.");
        scheduler.commitSubmittedFrame(RenderSequence{3});

        current.surface = SurfaceSignature{20};
        schedulerPlan = scheduler.beginFrame({RenderSequence{4}, current});
        require(targets(lumin::render::atmosphere::makeAtmosphereLutPassPlan(schedulerPlan)) ==
                    std::vector<AtmosphereLut>{AtmosphereLut::MultiScattering, AtmosphereLut::SkyView,
                                               AtmosphereLut::AerialPerspective},
                "A surface change must create the multi-scattering dependency closure.");
        scheduler.abandonFrame(RenderSequence{4});
    }

    void testForceRebuildProducesCompletePassTopology() {
        AtmosphereLutScheduler scheduler;
        const AtmosphereLutSignatures current = signatures();
        scheduler.commitSubmittedFrame(
            scheduler.beginFrame(AtmosphereLutFrameInput{RenderSequence{10}, current}).sequence());

        const auto schedulerPlan = scheduler.beginFrame(AtmosphereLutFrameInput{
            .sequence = RenderSequence{12},
            .signatures = current,
            .forceRebuild = true,
        });
        const auto passPlan = lumin::render::atmosphere::makeAtmosphereLutPassPlan(schedulerPlan);
        require(targets(passPlan) == std::vector<AtmosphereLut>{AtmosphereLut::Transmittance,
                                                                AtmosphereLut::MultiScattering, AtmosphereLut::SkyView,
                                                                AtmosphereLut::AerialPerspective},
                "A forced rebuild must restore the complete atmosphere compute topology.");
        scheduler.abandonFrame(RenderSequence{12});
    }

    void testFrameGraphRegistrationPreservesComputeOrder() {
        AtmosphereLutScheduler scheduler;
        const auto schedulerPlan = scheduler.beginFrame({RenderSequence{7}, signatures()});
        const auto plan = lumin::render::atmosphere::makeAtmosphereLutPassPlan(schedulerPlan);

        FrameGraph graph;
        AtmosphereLutFrameGraphResources resources;
        for (std::size_t index = 0; index < resources.textures.size(); ++index) {
            resources.textures[index] = graph.createTexture("atmosphere-lut", FrameGraphTextureDesc{});
        }
        resources.constants = graph.createBuffer("atmosphere-constants", lumin::render::FrameGraphBufferDesc{});

        requireThrows<std::invalid_argument>(
            [&] {
                (void)lumin::render::atmosphere::registerAtmosphereLutPasses(graph, plan, resources);
            },
            "A non-empty LUT plan must reject missing compute dispatch callbacks.");

        lumin::render::atmosphere::AtmosphereLutExecuteCallbacks callbacks;
        for (auto& callback : callbacks.callbacks) {
            callback = [](const lumin::render::FrameGraphContext&) {
            };
        }
        const auto registered =
            lumin::render::atmosphere::registerAtmosphereLutPasses(graph, plan, resources, std::move(callbacks));
        require(registered.size() == 4, "A full plan must register four compute passes.");
        graph.compile();
        const auto order = graph.executionOrder();
        require(std::vector<std::uint32_t>{order.begin(), order.end()} == std::vector<std::uint32_t>{0, 1, 2, 3},
                "FrameGraph must preserve the stable atmosphere branch order.");
        require(graph.passName(registered.pass(AtmosphereLut::Transmittance)) == "Atmosphere.Transmittance" &&
                    graph.passName(registered.pass(AtmosphereLut::MultiScattering)) == "Atmosphere.MultiScattering" &&
                    graph.passName(registered.pass(AtmosphereLut::SkyView)) == "Atmosphere.SkyView" &&
                    graph.passName(registered.pass(AtmosphereLut::AerialPerspective)) == "Atmosphere.AerialPerspective",
                "Atmosphere FrameGraph pass names are unstable.");
        scheduler.abandonFrame(RenderSequence{7});
    }

} // namespace

int main() {
    try {
        testDefaultResourceContractAndMemoryEstimate();
        testInvalidQualityIsRejected();
        testStableTopologyAndExplicitResourceUses();
        testMinimalPassSets();
        testForceRebuildProducesCompletePassTopology();
        testFrameGraphRegistrationPreservesComputeOrder();
        std::cout << "Atmosphere LUT pass tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Atmosphere LUT pass test failed: " << exception.what() << '\n';
        return 1;
    }
}

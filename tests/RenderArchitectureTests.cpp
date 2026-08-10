#include "render/core/FrameIdentity.hpp"
#include "render/core/History.hpp"
#include "render/core/RenderBlackboard.hpp"
#include "render/core/RenderCapabilities.hpp"
#include "render/core/RenderFeaturePlan.hpp"
#include "render/world/RenderWorld.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

    using namespace lumin::render::core;

    template <typename Left, typename Right>
    concept DirectlyEqualityComparable = requires(const Left& left, const Right& right) {
        { left == right } -> std::same_as<bool>;
    };

    static_assert(!std::is_convertible_v<FrameSlotIndex, std::uint32_t>);
    static_assert(!std::is_convertible_v<SwapImageIndex, std::uint32_t>);
    static_assert(!std::is_convertible_v<RenderSequence, std::uint64_t>);
    static_assert(!DirectlyEqualityComparable<FrameSlotIndex, SwapImageIndex>);
    static_assert(!std::is_copy_constructible_v<RenderBlackboard>);
    static_assert(std::is_nothrow_move_constructible_v<RenderBlackboard>);

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

    std::size_t featurePosition(const std::vector<FeatureId>& order, const char* id) {
        const auto iterator = std::find(order.begin(), order.end(), FeatureId{id});
        require(iterator != order.end(), std::string("Feature is absent from topological order: ") + id);
        return static_cast<std::size_t>(std::distance(order.begin(), iterator));
    }

    void testFrameIdentityUsesIndependentStrongTypes() {
        const FrameSlotIndex frameSlot{1};
        const SwapImageIndex swapImage{2};
        const RenderSequence sequence{42};
        const RenderExtent extent{1920, 1080};
        const RenderFrameIdentity identity{frameSlot, swapImage, sequence, extent};

        require(identity.isValid(), "A fully initialized frame identity must be valid.");
        require(frameSlot.value() == 1 && swapImage.value() == 2 && sequence.value() == 42,
                "Strong frame identifiers must retain their underlying values.");
        require(extent.pixelCount() == 2'073'600 && !extent.isEmpty(),
                "Render extent must compute its pixel count without truncation.");
        require(!RenderFrameIdentity{}.isValid(), "A default frame identity must be invalid.");
        require(RenderExtent{0, 1080}.isEmpty(), "A zero dimension must make a render extent empty.");
    }

    void testCapabilitySetAlgebra() {
        CapabilitySet base{RenderCapability::Graphics, RenderCapability::Compute, RenderCapability::DynamicRendering};
        const CapabilitySet rayTracing{RenderCapability::AccelerationStructure, RenderCapability::RayTracingPipeline};
        base.add(RenderCapability::DescriptorIndexing).remove(RenderCapability::Compute);

        require(base.contains(RenderCapability::Graphics) && !base.contains(RenderCapability::Compute),
                "Capability add and remove operations must update membership.");
        require(base.size() == 3, "Capability sets must not count duplicate or removed capabilities.");
        require((base | rayTracing).containsAll(rayTracing), "Capability union must contain both input sets.");
        require((base & rayTracing).empty(), "Disjoint capability intersection must be empty.");
        require((base | rayTracing).difference(base) == rayTracing,
                "Capability difference must retain only capabilities absent from the right operand.");

        const RenderDeviceCapabilities device{
            .supported = base | rayTracing, .maxFramesInFlight = 3, .maxRayRecursionDepth = 2};
        require(device.supports(RenderCapability::RayTracingPipeline) && device.supportsAll(rayTracing),
                "Device capability queries must delegate to the capability set.");
        require(renderCapabilityName(RenderCapability::AccelerationStructure) == "AccelerationStructure",
                "Capability names must remain stable for diagnostics.");

        CapabilitySet invalidCapability;
        invalidCapability.add(static_cast<RenderCapability>(255));
        require(invalidCapability.empty(), "Out-of-range capability values must be ignored without shifting bits.");
    }

    RenderFeaturePlan makeRepresentativeFeaturePlan() {
        RenderFeaturePlan plan;

        FeatureDescriptor composite{FeatureId{"composite"}};
        composite.dependencies = {FeatureId{"blinn-phong"}};
        plan.addFeature(std::move(composite));

        FeatureDescriptor rayGi{FeatureId{"ray-gi"}};
        rayGi.requiredCapabilities = {RenderCapability::AccelerationStructure, RenderCapability::RayTracingPipeline};
        rayGi.optionalCapabilities = {RenderCapability::ShaderFloat16};
        rayGi.dependencies = {FeatureId{"gbuffer"}};
        rayGi.historyDomains = {HistoryDomain::Sharc};
        plan.addFeature(std::move(rayGi));

        FeatureDescriptor blinnPhong{FeatureId{"blinn-phong"}};
        blinnPhong.requiredCapabilities = {RenderCapability::Graphics};
        blinnPhong.dependencies = {FeatureId{"gbuffer"}};
        plan.addFeature(std::move(blinnPhong));

        FeatureDescriptor gbuffer{FeatureId{"gbuffer"}};
        gbuffer.requiredCapabilities = {RenderCapability::Graphics, RenderCapability::DynamicRendering};
        plan.addFeature(std::move(gbuffer));

        FeatureDescriptor nrd{FeatureId{"nrd"}};
        nrd.requiredCapabilities = {RenderCapability::Compute, RenderCapability::Nrd};
        nrd.dependencies = {FeatureId{"ray-gi"}};
        nrd.historyDomains = {HistoryDomain::NrdDiffuse, HistoryDomain::NrdSpecular};
        plan.addFeature(std::move(nrd));

        FeatureDescriptor atmosphere{FeatureId{"atmosphere"}};
        atmosphere.requiredCapabilities = {RenderCapability::Compute};
        atmosphere.optionalCapabilities = {RenderCapability::ShaderFloat16};
        atmosphere.historyDomains = {HistoryDomain::AtmosphereLut};
        plan.addFeature(std::move(atmosphere));
        return plan;
    }

    void testFeaturePlanTopologyAndCapabilityFallback() {
        const RenderFeaturePlan plan = makeRepresentativeFeaturePlan();
        plan.validate();
        const std::vector<FeatureId> order = plan.topologicalOrder();
        require(featurePosition(order, "gbuffer") < featurePosition(order, "blinn-phong") &&
                    featurePosition(order, "blinn-phong") < featurePosition(order, "composite") &&
                    featurePosition(order, "gbuffer") < featurePosition(order, "ray-gi") &&
                    featurePosition(order, "ray-gi") < featurePosition(order, "nrd"),
                "Feature planning must place every strong dependency before its dependant.");
        require(order == plan.topologicalOrder(), "Feature topological order must be deterministic.");

        const RenderDeviceCapabilities rasterDevice{
            .supported = {RenderCapability::Graphics, RenderCapability::Compute, RenderCapability::DynamicRendering,
                          RenderCapability::ShaderFloat16, RenderCapability::Nrd},
            .maxFramesInFlight = 2,
        };
        const ResolvedRenderFeaturePlan resolved = plan.resolve(rasterDevice);
        const ResolvedRenderFeature* rayGi = resolved.find(FeatureId{"ray-gi"});
        const ResolvedRenderFeature* nrd = resolved.find(FeatureId{"nrd"});
        const ResolvedRenderFeature* atmosphere = resolved.find(FeatureId{"atmosphere"});
        require(rayGi != nullptr && rayGi->activation == FeatureActivation::DisabledMissingCapabilities &&
                    rayGi->missingRequiredCapabilities.contains(RenderCapability::AccelerationStructure),
                "A feature with missing required capabilities must be disabled with diagnostics.");
        require(nrd != nullptr && nrd->activation == FeatureActivation::DisabledDependency &&
                    nrd->unavailableDependency == FeatureId{"ray-gi"},
                "A disabled feature must disable its strong dependants.");
        require(atmosphere != nullptr && atmosphere->enabled() &&
                    atmosphere->availableOptionalCapabilities.contains(RenderCapability::ShaderFloat16),
                "Available optional capabilities must be reported without gating a feature.");
        require(atmosphere->historyDomains == std::vector{HistoryDomain::AtmosphereLut},
                "Resolved features must retain their history ownership contract.");
        require(resolved.executionOrder().size() == 4,
                "Only the four raster-compatible features must remain in the execution order.");

        std::unordered_map<FeatureId, int, FeatureIdHash> registry;
        registry.emplace(FeatureId{"gbuffer"}, 1);
        require(registry.at(FeatureId{"gbuffer"}) == 1,
                "Feature ids must provide a reusable content-based hash contract.");
    }

    void testFeaturePlanRejectsUnavailableAndMalformedContracts() {
        RenderFeaturePlan strictPlan;
        FeatureDescriptor sharc{FeatureId{"sharc"}};
        sharc.requiredCapabilities = {RenderCapability::Sharc};
        sharc.missingRequirementPolicy = MissingRequirementPolicy::RejectPlan;
        strictPlan.addFeature(std::move(sharc));
        requireThrows<std::runtime_error>(
            [&] {
                (void)strictPlan.resolve(RenderDeviceCapabilities{});
            },
            "RejectPlan must reject a plan when required capabilities are unavailable.");

        requireThrows<std::invalid_argument>(
            [] {
                (void)FeatureId{""};
            },
            "Feature ids must reject empty strings.");

        RenderFeaturePlan duplicateIds;
        duplicateIds.addFeature(FeatureDescriptor{FeatureId{"same"}});
        duplicateIds.addFeature(FeatureDescriptor{FeatureId{"same"}});
        requireThrows<std::invalid_argument>(
            [&] {
                duplicateIds.validate();
            },
            "Feature plans must reject duplicate ids.");

        RenderFeaturePlan missingDependency;
        FeatureDescriptor orphan{FeatureId{"orphan"}};
        orphan.dependencies = {FeatureId{"absent"}};
        missingDependency.addFeature(std::move(orphan));
        requireThrows<std::invalid_argument>(
            [&] {
                missingDependency.validate();
            },
            "Feature plans must reject missing dependency references.");

        RenderFeaturePlan cycle;
        FeatureDescriptor first{FeatureId{"first"}};
        first.dependencies = {FeatureId{"second"}};
        cycle.addFeature(std::move(first));
        FeatureDescriptor second{FeatureId{"second"}};
        second.dependencies = {FeatureId{"first"}};
        cycle.addFeature(std::move(second));
        requireThrows<std::invalid_argument>(
            [&] {
                cycle.validate();
            },
            "Feature plans must reject dependency cycles.");

        RenderFeaturePlan conflictingCapabilities;
        FeatureDescriptor conflict{FeatureId{"conflict"}};
        conflict.requiredCapabilities = {RenderCapability::Compute};
        conflict.optionalCapabilities = {RenderCapability::Compute};
        conflictingCapabilities.addFeature(std::move(conflict));
        requireThrows<std::invalid_argument>(
            [&] {
                conflictingCapabilities.validate();
            },
            "Required and optional capability sets must not overlap.");

        RenderFeaturePlan duplicateDomains;
        FeatureDescriptor temporal{FeatureId{"temporal"}};
        temporal.historyDomains = {HistoryDomain::Taa, HistoryDomain::Taa};
        duplicateDomains.addFeature(std::move(temporal));
        requireThrows<std::invalid_argument>(
            [&] {
                duplicateDomains.validate();
            },
            "Feature plans must reject duplicate history domains.");

        RenderFeaturePlan duplicateDomainOwners;
        FeatureDescriptor taaOwner{FeatureId{"taa-owner"}};
        taaOwner.historyDomains = {HistoryDomain::Taa};
        duplicateDomainOwners.addFeature(std::move(taaOwner));
        FeatureDescriptor secondTaaOwner{FeatureId{"second-taa-owner"}};
        secondTaaOwner.historyDomains = {HistoryDomain::Taa};
        duplicateDomainOwners.addFeature(std::move(secondTaaOwner));
        requireThrows<std::invalid_argument>(
            [&] {
                duplicateDomainOwners.validate();
            },
            "Each history domain must have one owning feature.");
    }

    void testHistoryPoliciesAreDomainSpecific() {
        FrameChangeSet changes;
        require(changes.empty(), "A default frame change set must be empty.");
        changes.add(HistoryReason::CameraCut).add(HistoryReason::LightingChanged);
        require(changes.containsAll(HistoryReason::CameraCut | HistoryReason::LightingChanged),
                "Frame changes must accumulate independent reasons.");

        const HistoryPolicyMap policies = makeDefaultHistoryPolicyMap();
        require(policies.actionFor(HistoryDomain::Taa, changes) == HistoryAction::FullReset,
                "A camera cut must fully reset TAA even when another reason only requests a soft reset.");
        require(policies.actionFor(HistoryDomain::NrdDiffuse, changes) == HistoryAction::FullReset &&
                    policies.actionFor(HistoryDomain::NrdSpecular, changes) == HistoryAction::FullReset,
                "A camera cut must fully reset both NRD history domains.");
        require(policies.actionFor(HistoryDomain::Sharc, changes) == HistoryAction::SoftReset,
                "SHARC must ignore camera cuts while reacting softly to lighting changes.");
        require(policies.actionFor(HistoryDomain::AtmosphereLut, changes) == HistoryAction::Keep,
                "Atmosphere LUTs must not be rebuilt for camera or ordinary lighting changes.");

        const FrameChangeSet resized{HistoryReason::RenderExtentChanged};
        require(policies.actionFor(HistoryDomain::Taa, resized) == HistoryAction::FullReset &&
                    policies.actionFor(HistoryDomain::Sharc, resized) == HistoryAction::Keep,
                "Render extent changes must reset screen-space history but preserve world-space SHARC data.");
        const FrameChangeSet atmosphereChanged{HistoryReason::AtmosphereParametersChanged};
        require(policies.actionFor(HistoryDomain::AtmosphereLut, atmosphereChanged) == HistoryAction::FullReset,
                "Atmosphere parameter changes must rebuild atmosphere LUT history.");

        HistoryPolicyMap customPolicies;
        customPolicies.setPolicy(HistoryDomain::Sharc, HistoryReason::CameraCut | HistoryReason::SceneContentChanged,
                                 HistoryAction::SoftReset);
        require(customPolicies.policy(HistoryDomain::Sharc, HistoryReason::CameraCut) == HistoryAction::SoftReset,
                "Custom history mappings must support assigning multiple reasons at once.");
        requireThrows<std::invalid_argument>(
            [&] {
                customPolicies.setPolicy(HistoryDomain::Count, HistoryReason::CameraCut, HistoryAction::Keep);
            },
            "History policies must reject invalid domains.");
        requireThrows<std::invalid_argument>(
            [&] {
                customPolicies.setPolicy(HistoryDomain::Taa, HistoryReason::CameraCut, static_cast<HistoryAction>(255));
            },
            "History policies must reject invalid actions.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)customPolicies.policy(HistoryDomain::Taa,
                                            HistoryReason::CameraCut | HistoryReason::LightingChanged);
            },
            "Direct history policy queries must require one reason.");
        requireThrows<std::invalid_argument>(
            [&] {
                const FrameChangeSet unknown{static_cast<HistoryReason>(1U << 31U)};
                (void)customPolicies.actionFor(HistoryDomain::Taa, unknown);
            },
            "History action resolution must reject unknown reason bits.");

        changes.remove(HistoryReason::CameraCut);
        require(!changes.containsAny(HistoryReason::CameraCut) && changes.containsAny(HistoryReason::LightingChanged),
                "Frame change removal must preserve unrelated reasons.");
        changes.clear();
        require(changes.empty(), "Clearing frame changes must remove every reason.");
    }

    void testSceneChangesMapToHistoryReasons() {
        using lumin::render::world::SceneChangeMask;

        require(frameChangesFromScene(SceneChangeMask::None).empty(),
                "An empty render-world delta must not invalidate any history domain.");

        const FrameChangeSet geometry =
            frameChangesFromScene(SceneChangeMask::Geometry | SceneChangeMask::InstanceTopology);
        require(geometry.containsAll(HistoryReason::SceneTopologyChanged) &&
                    !geometry.containsAny(HistoryReason::SceneContentChanged),
                "Geometry and instance topology changes must break temporal correspondence.");

        const FrameChangeSet materialParameters = frameChangesFromScene(SceneChangeMask::TransformOrMaterial);
        require(materialParameters.containsAll(HistoryReason::SceneContentChanged) &&
                    !materialParameters.containsAny(HistoryReason::SceneTopologyChanged),
                "Transform and material parameter changes must retain reusable history with a soft content reset.");

        const FrameChangeSet materialBinding = frameChangesFromScene(SceneChangeMask::MaterialBinding);
        require(materialBinding.containsAll(HistoryReason::SceneTopologyChanged | HistoryReason::SceneContentChanged),
                "Material binding changes must report both changed content and broken descriptor correspondence.");

        const FrameChangeSet environment =
            frameChangesFromScene(SceneChangeMask::Lighting | SceneChangeMask::Atmosphere);
        require(environment.containsAll(HistoryReason::LightingChanged | HistoryReason::AtmosphereParametersChanged),
                "Lighting and atmosphere deltas must remain independently identifiable.");

        requireThrows<std::invalid_argument>(
            [] {
                (void)frameChangesFromScene(static_cast<SceneChangeMask>(1U << 7U));
            },
            "Unknown render-world delta bits must not silently enter history policy evaluation.");
    }

    void testHistoryCoordinatorAdvancesOnlyAfterSuccessfulSubmit() {
        using lumin::render::world::SceneChangeMask;

        HistoryCoordinator coordinator;
        const HistoryFrameObservation firstObservation{
            .sequence = RenderSequence{0},
            .cameraCutEpoch = 4,
            .renderExtent = RenderExtent{1280, 720},
            .changes = frameChangesFromScene(SceneChangeMask::Lighting),
        };
        const HistoryFramePlan firstPlan = coordinator.beginFrame(firstObservation);
        require(firstPlan.isValid() && firstPlan.changes().containsAny(HistoryReason::FirstFrame),
                "Every attempt before the first successful submit must be planned as the first frame.");
        for (std::size_t index = 0; index < static_cast<std::size_t>(HistoryDomain::Count); ++index) {
            require(firstPlan.actionFor(static_cast<HistoryDomain>(index)) == HistoryAction::FullReset,
                    "The first submitted frame must initialize every history domain.");
        }

        const HistoryDomainState untouchedTaa = coordinator.state(HistoryDomain::Taa);
        coordinator.abandonFrame(RenderSequence{0});
        require(coordinator.state(HistoryDomain::Taa) == untouchedTaa &&
                    !coordinator.lastSuccessfulSequence().isValid() &&
                    coordinator.pendingChanges().containsAny(HistoryReason::LightingChanged),
                "Abandoning a frame must preserve domain state and retain one-shot changes for retry.");

        const HistoryFramePlan retriedFirstPlan = coordinator.beginFrame(firstObservation);
        require(retriedFirstPlan.changes().containsAll(HistoryReason::FirstFrame | HistoryReason::LightingChanged),
                "A failed first frame must replay both first-frame initialization and its queued changes.");
        coordinator.commitSubmittedFrame(RenderSequence{0});
        require(!coordinator.hasActiveFrame() && coordinator.pendingChanges().empty() &&
                    coordinator.lastSuccessfulSequence() == RenderSequence{0},
                "A successful submit must atomically close the plan and consume queued changes.");

        for (std::size_t index = 0; index < static_cast<std::size_t>(HistoryDomain::Count); ++index) {
            const HistoryDomainState& state = coordinator.state(static_cast<HistoryDomain>(index));
            require(state.valid && state.resetEpoch == 1 && state.acceptedFrameCount == 1 &&
                        state.lastCommittedAction == HistoryAction::FullReset &&
                        state.lastSuccessfulSequence == RenderSequence{0},
                    "Every domain must advance exactly once after the first successful submit.");
        }

        const HistoryFramePlan stable = coordinator.beginFrame(HistoryFrameObservation{
            .sequence = RenderSequence{1},
            .cameraCutEpoch = 4,
            .renderExtent = RenderExtent{1280, 720},
        });
        require(stable.changes().empty() && stable.actionFor(HistoryDomain::Taa) == HistoryAction::Keep &&
                    stable.actionFor(HistoryDomain::NrdDiffuse) == HistoryAction::Keep &&
                    stable.actionFor(HistoryDomain::NrdSpecular) == HistoryAction::Keep &&
                    stable.actionFor(HistoryDomain::Sharc) == HistoryAction::Keep &&
                    stable.actionFor(HistoryDomain::AtmosphereLut) == HistoryAction::Keep,
                "An unchanged frame must preserve all independent histories.");
        coordinator.commitSubmittedFrame(RenderSequence{1});

        const HistoryDomainState taaBeforeFailedCut = coordinator.state(HistoryDomain::Taa);
        const HistoryDomainState sharcBeforeFailedCut = coordinator.state(HistoryDomain::Sharc);
        const HistoryFrameObservation cutObservation{
            .sequence = RenderSequence{2},
            .cameraCutEpoch = 5,
            .renderExtent = RenderExtent{1280, 720},
        };
        const HistoryFramePlan cameraCut = coordinator.beginFrame(cutObservation);
        require(cameraCut.changes().containsAny(HistoryReason::CameraCut) &&
                    cameraCut.actionFor(HistoryDomain::Taa) == HistoryAction::FullReset &&
                    cameraCut.actionFor(HistoryDomain::NrdDiffuse) == HistoryAction::FullReset &&
                    cameraCut.actionFor(HistoryDomain::NrdSpecular) == HistoryAction::FullReset &&
                    cameraCut.actionFor(HistoryDomain::Sharc) == HistoryAction::Keep &&
                    cameraCut.actionFor(HistoryDomain::AtmosphereLut) == HistoryAction::Keep,
                "A camera cut must reset screen-space histories without discarding world-space caches.");
        coordinator.abandonFrame(RenderSequence{2});
        require(coordinator.state(HistoryDomain::Taa) == taaBeforeFailedCut &&
                    coordinator.state(HistoryDomain::Sharc) == sharcBeforeFailedCut &&
                    coordinator.lastSuccessfulSequence() == RenderSequence{1},
                "A failed camera-cut frame must not advance any history domain or the accepted sequence.");

        const HistoryFramePlan retriedCut = coordinator.beginFrame(cutObservation);
        require(retriedCut.changes().containsAny(HistoryReason::CameraCut),
                "Camera cut epochs must be compared with the last successful submit, so retries see the cut again.");
        coordinator.commitSubmittedFrame(RenderSequence{2});
        require(coordinator.state(HistoryDomain::Taa).resetEpoch == taaBeforeFailedCut.resetEpoch + 1 &&
                    coordinator.state(HistoryDomain::Sharc).resetEpoch == sharcBeforeFailedCut.resetEpoch,
                "Committing a camera cut must advance only the domains that performed a full reset.");

        const HistoryDomainState nrdBeforeFailedResize = coordinator.state(HistoryDomain::NrdDiffuse);
        const HistoryFrameObservation resizeObservation{
            .sequence = RenderSequence{3},
            .cameraCutEpoch = 5,
            .renderExtent = RenderExtent{1920, 1080},
        };
        const HistoryFramePlan resized = coordinator.beginFrame(resizeObservation);
        require(resized.changes().containsAny(HistoryReason::RenderExtentChanged) &&
                    resized.actionFor(HistoryDomain::Taa) == HistoryAction::FullReset &&
                    resized.actionFor(HistoryDomain::NrdDiffuse) == HistoryAction::FullReset &&
                    resized.actionFor(HistoryDomain::NrdSpecular) == HistoryAction::FullReset &&
                    resized.actionFor(HistoryDomain::Sharc) == HistoryAction::Keep &&
                    resized.actionFor(HistoryDomain::AtmosphereLut) == HistoryAction::Keep,
                "A render resize must reset pixel histories while preserving SHARC and atmosphere LUTs.");
        coordinator.abandonFrame(RenderSequence{3});
        const HistoryFramePlan retriedResize = coordinator.beginFrame(resizeObservation);
        require(retriedResize.changes().containsAny(HistoryReason::RenderExtentChanged) &&
                    coordinator.state(HistoryDomain::NrdDiffuse) == nrdBeforeFailedResize,
                "A failed resize frame must retain the last submitted extent and leave NRD history unadvanced.");
        coordinator.commitSubmittedFrame(RenderSequence{3});
    }

    void testHistoryCoordinatorReplaysSceneChangesAndSeparatesDomains() {
        using lumin::render::world::SceneChangeMask;

        HistoryCoordinator coordinator;
        (void)coordinator.beginFrame(HistoryFrameObservation{
            .sequence = RenderSequence{10},
            .cameraCutEpoch = 0,
            .renderExtent = RenderExtent{800, 600},
        });
        coordinator.commitSubmittedFrame(RenderSequence{10});

        const HistoryFrameObservation geometryObservation{
            .sequence = RenderSequence{11},
            .cameraCutEpoch = 0,
            .renderExtent = RenderExtent{800, 600},
            .changes = frameChangesFromScene(SceneChangeMask::Geometry),
        };
        const HistoryFramePlan geometry = coordinator.beginFrame(geometryObservation);
        require(geometry.actionFor(HistoryDomain::Taa) == HistoryAction::FullReset &&
                    geometry.actionFor(HistoryDomain::NrdDiffuse) == HistoryAction::FullReset &&
                    geometry.actionFor(HistoryDomain::NrdSpecular) == HistoryAction::FullReset &&
                    geometry.actionFor(HistoryDomain::Sharc) == HistoryAction::FullReset &&
                    geometry.actionFor(HistoryDomain::AtmosphereLut) == HistoryAction::Keep,
                "Geometry changes must reset temporal and SHARC histories without rebuilding atmosphere LUTs.");
        coordinator.abandonFrame(RenderSequence{11});

        const HistoryFramePlan replayedGeometry = coordinator.beginFrame(HistoryFrameObservation{
            .sequence = RenderSequence{11},
            .cameraCutEpoch = 0,
            .renderExtent = RenderExtent{800, 600},
        });
        require(replayedGeometry.changes().containsAny(HistoryReason::SceneTopologyChanged) &&
                    replayedGeometry.actionFor(HistoryDomain::Sharc) == HistoryAction::FullReset,
                "A one-shot scene delta must remain pending until a submission succeeds.");
        coordinator.commitSubmittedFrame(RenderSequence{11});

        const HistoryFramePlan material = coordinator.beginFrame(HistoryFrameObservation{
            .sequence = RenderSequence{12},
            .cameraCutEpoch = 0,
            .renderExtent = RenderExtent{800, 600},
            .changes = frameChangesFromScene(SceneChangeMask::TransformOrMaterial),
        });
        require(material.actionFor(HistoryDomain::Taa) == HistoryAction::SoftReset &&
                    material.actionFor(HistoryDomain::NrdDiffuse) == HistoryAction::SoftReset &&
                    material.actionFor(HistoryDomain::NrdSpecular) == HistoryAction::SoftReset &&
                    material.actionFor(HistoryDomain::Sharc) == HistoryAction::SoftReset &&
                    material.actionFor(HistoryDomain::AtmosphereLut) == HistoryAction::Keep,
                "Material and transform changes must soften temporal reuse without rebuilding atmosphere LUTs.");
        coordinator.commitSubmittedFrame(RenderSequence{12});

        const HistoryFramePlan lighting = coordinator.beginFrame(HistoryFrameObservation{
            .sequence = RenderSequence{13},
            .cameraCutEpoch = 0,
            .renderExtent = RenderExtent{800, 600},
            .changes = frameChangesFromScene(SceneChangeMask::Lighting),
        });
        require(lighting.actionFor(HistoryDomain::Taa) == HistoryAction::SoftReset &&
                    lighting.actionFor(HistoryDomain::NrdDiffuse) == HistoryAction::SoftReset &&
                    lighting.actionFor(HistoryDomain::NrdSpecular) == HistoryAction::SoftReset &&
                    lighting.actionFor(HistoryDomain::Sharc) == HistoryAction::SoftReset &&
                    lighting.actionFor(HistoryDomain::AtmosphereLut) == HistoryAction::Keep,
                "Lighting changes must soften radiance histories without invalidating atmosphere LUT inputs.");
        coordinator.commitSubmittedFrame(RenderSequence{13});

        const HistoryFramePlan atmosphere = coordinator.beginFrame(HistoryFrameObservation{
            .sequence = RenderSequence{14},
            .cameraCutEpoch = 0,
            .renderExtent = RenderExtent{800, 600},
            .changes = frameChangesFromScene(SceneChangeMask::Atmosphere),
        });
        require(atmosphere.actionFor(HistoryDomain::Taa) == HistoryAction::SoftReset &&
                    atmosphere.actionFor(HistoryDomain::NrdDiffuse) == HistoryAction::SoftReset &&
                    atmosphere.actionFor(HistoryDomain::NrdSpecular) == HistoryAction::SoftReset &&
                    atmosphere.actionFor(HistoryDomain::Sharc) == HistoryAction::SoftReset &&
                    atmosphere.actionFor(HistoryDomain::AtmosphereLut) == HistoryAction::FullReset,
                "Atmosphere parameter changes must rebuild LUTs while softly adapting radiance histories.");
        coordinator.commitSubmittedFrame(RenderSequence{14});
    }

    void testHistoryCoordinatorRejectsInvalidTransactions() {
        HistoryCoordinator coordinator;
        requireThrows<std::invalid_argument>(
            [&] {
                (void)coordinator.beginFrame(HistoryFrameObservation{
                    .sequence = RenderSequence{},
                    .renderExtent = RenderExtent{640, 480},
                });
            },
            "History coordination must reject an invalid render sequence.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)coordinator.beginFrame(HistoryFrameObservation{
                    .sequence = RenderSequence{0},
                    .renderExtent = RenderExtent{},
                });
            },
            "History coordination must reject an empty render extent.");

        (void)coordinator.beginFrame(HistoryFrameObservation{
            .sequence = RenderSequence{0},
            .renderExtent = RenderExtent{640, 480},
        });
        requireThrows<std::logic_error>(
            [&] {
                (void)coordinator.beginFrame(HistoryFrameObservation{
                    .sequence = RenderSequence{1},
                    .renderExtent = RenderExtent{640, 480},
                });
            },
            "A second frame must not start before the active transaction is closed.");
        requireThrows<std::logic_error>(
            [&] {
                coordinator.commitSubmittedFrame(RenderSequence{1});
            },
            "A stale completion must not commit another frame's history plan.");
        coordinator.commitSubmittedFrame(RenderSequence{0});

        requireThrows<std::logic_error>(
            [&] {
                coordinator.abandonFrame(RenderSequence{1});
            },
            "Completing history without an active frame must be rejected.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)coordinator.beginFrame(HistoryFrameObservation{
                    .sequence = RenderSequence{0},
                    .renderExtent = RenderExtent{640, 480},
                });
            },
            "A successful render sequence must never be reused.");
    }

    struct MoveOnlyPayload {
        explicit MoveOnlyPayload(int value) : value(std::make_unique<int>(value)) {
        }

        MoveOnlyPayload(const MoveOnlyPayload&) = delete;
        MoveOnlyPayload& operator=(const MoveOnlyPayload&) = delete;
        MoveOnlyPayload(MoveOnlyPayload&&) noexcept = default;
        MoveOnlyPayload& operator=(MoveOnlyPayload&&) noexcept = default;

        std::unique_ptr<int> value;
    };

    void testRenderBlackboardTypeSafetyAndReplacement() {
        RenderBlackboard blackboard;
        require(blackboard.empty(), "A default render blackboard must be empty.");

        int& storedInteger = blackboard.set(7);
        require(storedInteger == 7 && blackboard.contains<int>() && !blackboard.contains<float>(),
                "Blackboard set and contains operations must use exact concrete types.");
        blackboard.set(11);
        blackboard.set(std::string{"frame-data"});
        require(blackboard.get<int>() == 11 && blackboard.get<std::string>() == "frame-data" && blackboard.size() == 2,
                "Setting an existing type must replace it without adding a second entry.");

        MoveOnlyPayload& payload = blackboard.emplace<MoveOnlyPayload>(23);
        require(payload.value != nullptr && *payload.value == 23,
                "Blackboard emplacement must support move-only payloads.");
        blackboard.set(MoveOnlyPayload{29});
        require(*blackboard.get<MoveOnlyPayload>().value == 29,
                "Blackboard replacement must support move-only payloads.");

        const RenderBlackboard& readonly = blackboard;
        require(readonly.tryGet<int>() != nullptr && *readonly.tryGet<int>() == 11,
                "A const blackboard must provide read-only typed lookup.");
        require(readonly.tryGet<double>() == nullptr, "Blackboard tryGet must return null for an absent type.");
        requireThrows<std::out_of_range>(
            [&] {
                (void)blackboard.get<double>();
            },
            "Blackboard get must throw for an absent type.");

        require(blackboard.erase<std::string>() && !blackboard.erase<std::string>(),
                "Blackboard erase must report whether an entry existed.");
        blackboard.clear();
        require(blackboard.empty(), "Blackboard clear must destroy every stored type.");
    }

} // namespace

int main() {
    try {
        testFrameIdentityUsesIndependentStrongTypes();
        testCapabilitySetAlgebra();
        testFeaturePlanTopologyAndCapabilityFallback();
        testFeaturePlanRejectsUnavailableAndMalformedContracts();
        testHistoryPoliciesAreDomainSpecific();
        testSceneChangesMapToHistoryReasons();
        testHistoryCoordinatorAdvancesOnlyAfterSuccessfulSubmit();
        testHistoryCoordinatorReplaysSceneChangesAndSeparatesDomains();
        testHistoryCoordinatorRejectsInvalidTransactions();
        testRenderBlackboardTypeSafetyAndReplacement();
        std::cout << "Render architecture tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Render architecture test failed: " << exception.what() << '\n';
        return 1;
    }
}

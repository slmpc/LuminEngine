#include "render/core/History.hpp"

#include "render/world/RenderWorld.hpp"

#include <array>
#include <bit>
#include <stdexcept>
#include <utility>

namespace lumin::render::core {

    namespace {

        constexpr std::array knownReasons = {
            HistoryReason::FirstFrame,           HistoryReason::CameraCut,
            HistoryReason::RenderExtentChanged,  HistoryReason::SwapchainRecreated,
            HistoryReason::SceneTopologyChanged, HistoryReason::SceneContentChanged,
            HistoryReason::LightingChanged,      HistoryReason::FeatureConfigurationChanged,
            HistoryReason::ShaderReloaded,       HistoryReason::AtmosphereParametersChanged,
            HistoryReason::WorldOriginRebased,   HistoryReason::DeviceRecovered,
        };

        [[nodiscard]] std::size_t domainIndex(HistoryDomain domain) {
            const std::size_t index = static_cast<std::size_t>(domain);
            if (index >= static_cast<std::size_t>(HistoryDomain::Count)) {
                throw std::invalid_argument("History domain is invalid.");
            }
            return index;
        }

        [[nodiscard]] std::size_t singleReasonIndex(HistoryReason reason) {
            const std::uint32_t bits = static_cast<std::uint32_t>(reason);
            const std::uint32_t knownBits = static_cast<std::uint32_t>(HistoryReason::All);
            if (bits == 0 || (bits & ~knownBits) != 0 || std::popcount(bits) != 1) {
                throw std::invalid_argument("History reason must contain exactly one known reason.");
            }
            return static_cast<std::size_t>(std::countr_zero(bits));
        }

        void setTemporalPolicies(HistoryPolicyMap& policies, HistoryDomain domain) {
            policies.setPolicy(domain,
                               HistoryReason::FirstFrame | HistoryReason::CameraCut |
                                   HistoryReason::RenderExtentChanged | HistoryReason::SwapchainRecreated |
                                   HistoryReason::SceneTopologyChanged | HistoryReason::FeatureConfigurationChanged |
                                   HistoryReason::ShaderReloaded | HistoryReason::WorldOriginRebased |
                                   HistoryReason::DeviceRecovered,
                               HistoryAction::FullReset);
            policies.setPolicy(domain,
                               HistoryReason::SceneContentChanged | HistoryReason::LightingChanged |
                                   HistoryReason::AtmosphereParametersChanged,
                               HistoryAction::SoftReset);
        }

    } // namespace

    HistoryPolicyMap::HistoryPolicyMap() noexcept {
        for (auto& domain : policies_) {
            domain.fill(HistoryAction::Keep);
        }
    }

    void HistoryPolicyMap::setPolicy(HistoryDomain domain, HistoryReason reasons, HistoryAction action) {
        const std::size_t selectedDomain = domainIndex(domain);
        if (static_cast<std::uint8_t>(action) > static_cast<std::uint8_t>(HistoryAction::FullReset)) {
            throw std::invalid_argument("History action is invalid.");
        }
        const std::uint32_t bits = static_cast<std::uint32_t>(reasons);
        const std::uint32_t knownBits = static_cast<std::uint32_t>(HistoryReason::All);
        if (bits == 0 || (bits & ~knownBits) != 0) {
            throw std::invalid_argument("History reasons must contain one or more known reasons.");
        }

        for (std::size_t index = 0; index < knownReasons.size(); ++index) {
            if ((reasons & knownReasons[index]) != HistoryReason::None) {
                policies_[selectedDomain][index] = action;
            }
        }
    }

    HistoryAction HistoryPolicyMap::policy(HistoryDomain domain, HistoryReason reason) const {
        return policies_[domainIndex(domain)][singleReasonIndex(reason)];
    }

    HistoryAction HistoryPolicyMap::actionFor(HistoryDomain domain, const FrameChangeSet& changes) const {
        const std::size_t selectedDomain = domainIndex(domain);
        const std::uint32_t bits = static_cast<std::uint32_t>(changes.reasons());
        const std::uint32_t knownBits = static_cast<std::uint32_t>(HistoryReason::All);
        if ((bits & ~knownBits) != 0) {
            throw std::invalid_argument("Frame changes contain an unknown history reason.");
        }
        HistoryAction result = HistoryAction::Keep;
        for (std::size_t index = 0; index < knownReasons.size(); ++index) {
            if (changes.containsAny(knownReasons[index])) {
                result = strongerHistoryAction(result, policies_[selectedDomain][index]);
            }
        }
        return result;
    }

    HistoryPolicyMap makeDefaultHistoryPolicyMap() {
        HistoryPolicyMap policies;
        setTemporalPolicies(policies, HistoryDomain::Taa);
        setTemporalPolicies(policies, HistoryDomain::NrdDiffuse);
        setTemporalPolicies(policies, HistoryDomain::NrdSpecular);

        policies.setPolicy(HistoryDomain::Sharc,
                           HistoryReason::FirstFrame | HistoryReason::SceneTopologyChanged |
                               HistoryReason::FeatureConfigurationChanged | HistoryReason::ShaderReloaded |
                               HistoryReason::WorldOriginRebased | HistoryReason::DeviceRecovered,
                           HistoryAction::FullReset);
        policies.setPolicy(HistoryDomain::Sharc,
                           HistoryReason::SceneContentChanged | HistoryReason::LightingChanged |
                               HistoryReason::AtmosphereParametersChanged,
                           HistoryAction::SoftReset);

        policies.setPolicy(HistoryDomain::AtmosphereLut,
                           HistoryReason::FirstFrame | HistoryReason::FeatureConfigurationChanged |
                               HistoryReason::ShaderReloaded | HistoryReason::AtmosphereParametersChanged |
                               HistoryReason::DeviceRecovered,
                           HistoryAction::FullReset);
        return policies;
    }

    FrameChangeSet frameChangesFromScene(world::SceneChangeMask sceneChanges) {
        const std::uint8_t bits = static_cast<std::uint8_t>(sceneChanges);
        const std::uint8_t knownBits = static_cast<std::uint8_t>(world::SceneChangeMask::All);
        if ((bits & static_cast<std::uint8_t>(~knownBits)) != 0) {
            throw std::invalid_argument("Scene changes contain an unknown bit.");
        }

        FrameChangeSet result;
        const world::SceneChangeMask correspondenceBreakingChanges = world::SceneChangeMask::Geometry |
                                                                     world::SceneChangeMask::InstanceTopology |
                                                                     world::SceneChangeMask::MaterialBinding;
        if (world::hasAnyChange(sceneChanges, correspondenceBreakingChanges)) {
            result.add(HistoryReason::SceneTopologyChanged);
        }
        if (world::hasAnyChange(sceneChanges, world::SceneChangeMask::TransformOrMaterial |
                                                  world::SceneChangeMask::MaterialBinding)) {
            result.add(HistoryReason::SceneContentChanged);
        }
        if (world::hasAnyChange(sceneChanges, world::SceneChangeMask::Lighting)) {
            result.add(HistoryReason::LightingChanged);
        }
        if (world::hasAnyChange(sceneChanges, world::SceneChangeMask::Atmosphere)) {
            result.add(HistoryReason::AtmosphereParametersChanged);
        }
        return result;
    }

    HistoryFramePlan::HistoryFramePlan(RenderSequence sequence, FrameChangeSet changes,
                                       std::array<HistoryAction, domainCount> actions) noexcept
        : sequence_(sequence), changes_(changes), actions_(actions) {
    }

    RenderSequence HistoryFramePlan::sequence() const noexcept {
        return sequence_;
    }

    const FrameChangeSet& HistoryFramePlan::changes() const noexcept {
        return changes_;
    }

    HistoryAction HistoryFramePlan::actionFor(HistoryDomain domain) const {
        return actions_[domainIndex(domain)];
    }

    bool HistoryFramePlan::isValid() const noexcept {
        return sequence_.isValid();
    }

    HistoryCoordinator::HistoryCoordinator() : HistoryCoordinator(makeDefaultHistoryPolicyMap()) {
    }

    HistoryCoordinator::HistoryCoordinator(HistoryPolicyMap policies) : policies_(std::move(policies)) {
    }

    HistoryFramePlan HistoryCoordinator::beginFrame(const HistoryFrameObservation& observation) {
        if (activePlan_) {
            throw std::logic_error("A history frame is already active.");
        }
        if (!observation.sequence.isValid()) {
            throw std::invalid_argument("History frame sequence is invalid.");
        }
        if (observation.renderExtent.isEmpty()) {
            throw std::invalid_argument("History frame render extent must not be empty.");
        }
        if (lastSuccessfulSequence_.isValid() && observation.sequence <= lastSuccessfulSequence_) {
            throw std::invalid_argument("History frame sequence must advance beyond the last successful submission.");
        }

        FrameChangeSet nextPendingChanges = pendingChanges_;
        nextPendingChanges.merge(observation.changes);
        FrameChangeSet effectiveChanges = nextPendingChanges;
        if (!lastSuccessfulSequence_.isValid()) {
            effectiveChanges.add(HistoryReason::FirstFrame);
        } else {
            if (observation.cameraCutEpoch != committedCameraCutEpoch_) {
                effectiveChanges.add(HistoryReason::CameraCut);
            }
            if (observation.renderExtent != committedRenderExtent_) {
                effectiveChanges.add(HistoryReason::RenderExtentChanged);
            }
        }

        std::array<HistoryAction, domainCount> actions{};
        for (std::size_t index = 0; index < domainCount; ++index) {
            actions[index] = policies_.actionFor(static_cast<HistoryDomain>(index), effectiveChanges);
        }

        HistoryFramePlan plan{observation.sequence, effectiveChanges, actions};
        pendingChanges_ = nextPendingChanges;
        activeObservation_ = observation;
        activePlan_ = plan;
        return plan;
    }

    void HistoryCoordinator::commitSubmittedFrame(RenderSequence sequence) {
        requireActiveSequence(sequence);

        for (std::size_t index = 0; index < domainCount; ++index) {
            HistoryDomainState& domainState = states_[index];
            const HistoryAction action = activePlan_->actionFor(static_cast<HistoryDomain>(index));
            domainState.valid = true;
            if (action == HistoryAction::FullReset) {
                ++domainState.resetEpoch;
            }
            ++domainState.acceptedFrameCount;
            domainState.lastCommittedAction = action;
            domainState.lastSuccessfulSequence = sequence;
        }

        lastSuccessfulSequence_ = sequence;
        committedCameraCutEpoch_ = activeObservation_->cameraCutEpoch;
        committedRenderExtent_ = activeObservation_->renderExtent;
        pendingChanges_.clear();
        activePlan_.reset();
        activeObservation_.reset();
    }

    void HistoryCoordinator::abandonFrame(RenderSequence sequence) {
        requireActiveSequence(sequence);
        activePlan_.reset();
        activeObservation_.reset();
    }

    bool HistoryCoordinator::hasActiveFrame() const noexcept {
        return activePlan_.has_value();
    }

    const HistoryDomainState& HistoryCoordinator::state(HistoryDomain domain) const {
        return states_[domainIndex(domain)];
    }

    RenderSequence HistoryCoordinator::lastSuccessfulSequence() const noexcept {
        return lastSuccessfulSequence_;
    }

    const FrameChangeSet& HistoryCoordinator::pendingChanges() const noexcept {
        return pendingChanges_;
    }

    void HistoryCoordinator::requireActiveSequence(RenderSequence sequence) const {
        if (!activePlan_) {
            throw std::logic_error("No history frame is active.");
        }
        if (activePlan_->sequence() != sequence) {
            throw std::logic_error("History frame sequence does not match the active plan.");
        }
    }

} // namespace lumin::render::core

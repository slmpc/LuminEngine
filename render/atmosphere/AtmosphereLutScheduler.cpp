#include "render/atmosphere/AtmosphereLutScheduler.hpp"

#include <stdexcept>

namespace lumin::render::atmosphere {
    namespace {

        constexpr std::size_t lutCount = static_cast<std::size_t>(AtmosphereLut::Count);

        [[nodiscard]] std::size_t lutIndex(AtmosphereLut lut) {
            const std::size_t index = static_cast<std::size_t>(lut);
            if (index >= lutCount) {
                throw std::invalid_argument("Atmosphere LUT is invalid.");
            }
            return index;
        }

        [[nodiscard]] std::uint64_t nextGeneration(std::uint64_t generation) noexcept {
            ++generation;
            return generation == 0 ? 1 : generation;
        }

    } // namespace

    AtmosphereLutPlan::AtmosphereLutPlan(core::RenderSequence sequence, std::array<bool, lutCount> rebuild,
                                         std::array<std::uint64_t, lutCount> generations) noexcept
        : sequence_(sequence), rebuild_(rebuild), generations_(generations) {
    }

    core::RenderSequence AtmosphereLutPlan::sequence() const noexcept {
        return sequence_;
    }

    bool AtmosphereLutPlan::rebuilds(AtmosphereLut lut) const {
        return rebuild_[lutIndex(lut)];
    }

    std::uint64_t AtmosphereLutPlan::generationAfterSubmit(AtmosphereLut lut) const {
        return generations_[lutIndex(lut)];
    }

    bool AtmosphereLutPlan::isValid() const noexcept {
        return sequence_.isValid();
    }

    AtmosphereLutPlan AtmosphereLutScheduler::beginFrame(const AtmosphereLutFrameInput& input) {
        if (activePlan_) {
            throw std::logic_error("An atmosphere LUT frame is already active.");
        }
        if (!input.sequence.isValid()) {
            throw std::invalid_argument("Atmosphere LUT frame sequence is invalid.");
        }
        if (!input.signatures.isValid()) {
            throw std::invalid_argument("Atmosphere LUT frame signatures are invalid.");
        }
        if (lastSuccessfulSequence_.isValid() && input.sequence <= lastSuccessfulSequence_) {
            throw std::invalid_argument(
                "Atmosphere LUT frame sequence must advance beyond the last successful submission.");
        }

        // 资源被外部生命周期重建后，即使输入签名相同，原 LUT 内容也不能继续复用。
        const bool rebuildAll = !committedSignatures_.has_value() || input.forceRebuild;
        const bool opticalChanged = rebuildAll || input.signatures.optical != committedSignatures_->optical;
        const bool surfaceChanged = rebuildAll || input.signatures.surface != committedSignatures_->surface;
        const bool lightingChanged = rebuildAll || input.signatures.lighting != committedSignatures_->lighting;
        const bool skyViewChanged = rebuildAll || input.signatures.skyView != committedSignatures_->skyView;
        const bool aerialViewChanged =
            rebuildAll || input.signatures.aerialPerspective != committedSignatures_->aerialPerspective;

        std::array<bool, lutCount> rebuild{};
        rebuild[lutIndex(AtmosphereLut::Transmittance)] = opticalChanged;
        rebuild[lutIndex(AtmosphereLut::MultiScattering)] =
            opticalChanged || surfaceChanged || rebuild[lutIndex(AtmosphereLut::Transmittance)];
        rebuild[lutIndex(AtmosphereLut::SkyView)] = opticalChanged || surfaceChanged || lightingChanged ||
                                                    skyViewChanged || rebuild[lutIndex(AtmosphereLut::MultiScattering)];
        rebuild[lutIndex(AtmosphereLut::AerialPerspective)] = opticalChanged || surfaceChanged || lightingChanged ||
                                                              aerialViewChanged ||
                                                              rebuild[lutIndex(AtmosphereLut::MultiScattering)];

        std::array<std::uint64_t, lutCount> generations{};
        for (std::size_t index = 0; index < lutCount; ++index) {
            generations[index] = rebuild[index] ? nextGeneration(states_[index].generation) : states_[index].generation;
        }

        AtmosphereLutPlan plan{input.sequence, rebuild, generations};
        activeInput_ = input;
        activePlan_ = plan;
        return plan;
    }

    void AtmosphereLutScheduler::commitSubmittedFrame(core::RenderSequence sequence) {
        requireActiveSequence(sequence);

        for (std::size_t index = 0; index < lutCount; ++index) {
            if (!activePlan_->rebuilds(static_cast<AtmosphereLut>(index))) {
                continue;
            }
            AtmosphereLutState& lutState = states_[index];
            lutState.valid = true;
            lutState.generation = activePlan_->generationAfterSubmit(static_cast<AtmosphereLut>(index));
            lutState.lastRebuildSequence = sequence;
        }

        committedSignatures_ = activeInput_->signatures;
        lastSuccessfulSequence_ = sequence;
        activePlan_.reset();
        activeInput_.reset();
    }

    void AtmosphereLutScheduler::abandonFrame(core::RenderSequence sequence) {
        requireActiveSequence(sequence);
        activePlan_.reset();
        activeInput_.reset();
    }

    bool AtmosphereLutScheduler::hasActiveFrame() const noexcept {
        return activePlan_.has_value();
    }

    const AtmosphereLutState& AtmosphereLutScheduler::state(AtmosphereLut lut) const {
        return states_[lutIndex(lut)];
    }

    core::RenderSequence AtmosphereLutScheduler::lastSuccessfulSequence() const noexcept {
        return lastSuccessfulSequence_;
    }

    const AtmosphereLutSignatures* AtmosphereLutScheduler::committedSignatures() const noexcept {
        return committedSignatures_ ? &*committedSignatures_ : nullptr;
    }

    void AtmosphereLutScheduler::requireActiveSequence(core::RenderSequence sequence) const {
        if (!activePlan_) {
            throw std::logic_error("No atmosphere LUT frame is active.");
        }
        if (activePlan_->sequence() != sequence) {
            throw std::logic_error("Atmosphere LUT frame completion does not match the active sequence.");
        }
    }

} // namespace lumin::render::atmosphere

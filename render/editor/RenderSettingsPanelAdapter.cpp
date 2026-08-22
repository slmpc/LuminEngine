#include "render/editor/RenderSettingsPanelAdapter.hpp"

#include "render/pipelines/DefaultRenderPipelines.hpp"

namespace lumin::render::editor {

    struct RenderSettingsPanelAdapter::State final {
        State() {
            pipelines::registerDefaultRenderSettings(schemas);
            store = std::make_unique<core::RenderSettingsStore>(schemas);
        }

        core::RenderSettingsSchemaRegistry schemas;
        std::unique_ptr<core::RenderSettingsStore> store;
    };

    RenderSettingsPanelAdapter::RenderSettingsPanelAdapter() : state_(std::make_unique<State>()) {
        editable_ = pipelines::readDefaultRenderSettings(state_->store->snapshot());
    }

    RenderSettingsPanelAdapter::~RenderSettingsPanelAdapter() = default;

    RenderSettings& RenderSettingsPanelAdapter::editable() noexcept {
        return editable_;
    }

    const RenderSettings& RenderSettingsPanelAdapter::editable() const noexcept {
        return editable_;
    }

    core::RenderSettingsSnapshot RenderSettingsPanelAdapter::snapshot() {
        state_->store->set(pipelines::feature_ids::shadow(), editable_.shadows);
        state_->store->set(pipelines::feature_ids::globalIllumination(), editable_.globalIllumination);
        state_->store->set(pipelines::feature_ids::lightingComposite(), editable_.directLighting);
        state_->store->set(pipelines::feature_ids::temporalAa(), editable_.temporalAa);
        state_->store->set(pipelines::feature_ids::toneMapping(), editable_.toneMapping);
        state_->store->set(pipelines::feature_ids::atmosphere(), editable_.atmosphere);
        return state_->store->snapshot();
    }

} // namespace lumin::render::editor

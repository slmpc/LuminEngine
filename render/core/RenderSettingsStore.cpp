#include "render/core/RenderSettingsStore.hpp"

namespace lumin::render::core {

    FeatureSettingsChange& FeatureSettingsChange::merge(const FeatureSettingsChange& other) noexcept {
        impact |= other.impact;
        historyReasons.merge(other.historyReasons);
        return *this;
    }

    RenderSettingsSnapshot::RenderSettingsSnapshot() : values_(std::make_shared<const Values>()) {
    }

    RenderSettingsSnapshot::RenderSettingsSnapshot(std::shared_ptr<const Values> values) : values_(std::move(values)) {
    }

    bool RenderSettingsSnapshot::contains(const FeatureId& id) const noexcept {
        return values_->contains(id);
    }

    bool RenderSettingsSchemaRegistry::contains(const FeatureId& id) const noexcept {
        return schemas_.contains(id);
    }

    std::size_t RenderSettingsSchemaRegistry::size() const noexcept {
        return schemas_.size();
    }

    FeatureSettingsChange RenderSettingsSchemaRegistry::diff(const RenderSettingsSnapshot& before,
                                                             const RenderSettingsSnapshot& after) const {
        FeatureSettingsChange result;
        for (const auto& [id, schema] : schemas_) {
            const auto beforeValue = before.values_->find(id);
            const auto afterValue = after.values_->find(id);
            if (beforeValue == before.values_->end() || afterValue == after.values_->end()) {
                throw std::invalid_argument("Render settings snapshots do not match the registered schema set.");
            }
            schema.validate(beforeValue->second);
            schema.validate(afterValue->second);
            result.merge(schema.diff(beforeValue->second, afterValue->second));
        }
        return result;
    }

    RenderSettingsStore::RenderSettingsStore(const RenderSettingsSchemaRegistry& schemas) : schemas_(&schemas) {
        values_.reserve(schemas.schemas_.size());
        for (const auto& [id, schema] : schemas.schemas_) {
            values_.emplace(id, schema.defaults);
        }
    }

    RenderSettingsSnapshot RenderSettingsStore::snapshot() const {
        return RenderSettingsSnapshot{std::make_shared<const RenderSettingsSnapshot::Values>(values_)};
    }

} // namespace lumin::render::core

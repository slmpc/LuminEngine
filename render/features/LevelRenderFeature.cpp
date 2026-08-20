#include "render/features/LevelRenderFeature.hpp"

#include <utility>

namespace lumin::render {

    LevelRenderFeature::LevelRenderFeature(LevelRenderFeatureKind kind, core::FeatureDescriptor descriptor,
                                           LevelRenderFeatureHost& host) noexcept
        : kind_(kind), descriptor_(std::move(descriptor)), host_(host) {
    }

    const core::FeatureDescriptor& LevelRenderFeature::descriptor() const noexcept {
        return descriptor_;
    }

    void LevelRenderFeature::addPasses(core::RenderFeatureFrameContext& context) {
        host_.addFeaturePasses(kind_, context);
    }

    void LevelRenderFeature::onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept {
        host_.submitFeature(kind_, identity);
    }

    void LevelRenderFeature::onFrameDiscarded(const core::RenderFrameIdentity& identity) noexcept {
        host_.discardFeature(kind_, identity);
    }

    std::unique_ptr<core::IRenderFeature>
    makeLevelRenderFeature(LevelRenderFeatureKind kind, core::FeatureDescriptor descriptor,
                           LevelRenderFeatureHost& host) {
        return std::make_unique<LevelRenderFeature>(kind, std::move(descriptor), host);
    }

} // namespace lumin::render

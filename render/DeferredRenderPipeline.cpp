#include "render/DeferredRenderPipeline.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace lumin::render {

    namespace {

        class DeferredCallbackFeature final : public core::IRenderFeature {
        public:
            DeferredCallbackFeature(core::FeatureDescriptor descriptor, DeferredRenderFeatureCallbacks callbacks)
                : descriptor_(std::move(descriptor)), callbacks_(std::move(callbacks)) {
                if (!callbacks_.addPasses) {
                    throw std::invalid_argument("Deferred render feature requires an addPasses callback.");
                }
            }

            [[nodiscard]] const core::FeatureDescriptor& descriptor() const noexcept override {
                return descriptor_;
            }

            void addPasses(core::RenderFeatureFrameContext& context) override {
                callbacks_.addPasses(context);
            }

            void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
                if (callbacks_.onSubmitted) {
                    callbacks_.onSubmitted(identity);
                }
            }

            void onFrameDiscarded(const core::RenderFrameIdentity& identity) noexcept override {
                if (callbacks_.onDiscarded) {
                    callbacks_.onDiscarded(identity);
                }
            }

        private:
            core::FeatureDescriptor descriptor_;
            DeferredRenderFeatureCallbacks callbacks_;
        };

        [[nodiscard]] core::FeatureDescriptor feature(std::string_view id,
                                                      std::initializer_list<std::string_view> dependencies = {},
                                                      std::initializer_list<core::HistoryDomain> histories = {}) {
            core::FeatureDescriptor descriptor{core::FeatureId{id}};
            descriptor.requiredCapabilities = {core::RenderCapability::Graphics,
                                               core::RenderCapability::DynamicRendering};
            for (const std::string_view dependency : dependencies) {
                descriptor.dependencies.emplace_back(dependency);
            }
            descriptor.historyDomains.assign(histories);
            return descriptor;
        }

    } // namespace

    DeferredRenderPipeline::DeferredRenderPipeline(DeferredRenderPipelineCallbacks callbacks,
                                                   const core::RenderDeviceCapabilities& capabilities,
                                                   DeferredRenderPath path)
        : path_(path) {
        if (path_ == DeferredRenderPath::Raster) {
            pipeline_.addFeature(
                std::make_unique<DeferredCallbackFeature>(feature("shadow"), std::move(callbacks.shadow)));
            pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(
                feature("gbuffer", {"shadow"}), std::move(callbacks.gbuffer)));
        }

        core::FeatureDescriptor atmosphere = feature("atmosphere-luts", {}, {core::HistoryDomain::AtmosphereLut});
        if (path_ == DeferredRenderPath::Raster) {
            atmosphere.dependencies.emplace_back("gbuffer");
        }
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(std::move(atmosphere),
                                                                        std::move(callbacks.atmosphereLuts)));

        if (path_ == DeferredRenderPath::Hybrid) {
            if (!callbacks.hybridSurface.addPasses) {
                throw std::invalid_argument("Hybrid render path requires a primary surface callback.");
            }
            core::FeatureDescriptor surface = feature("hybrid-surface", {"atmosphere-luts"});
            surface.requiredCapabilities.add(core::RenderCapability::AccelerationStructure)
                .add(core::RenderCapability::RayTracingPipeline);
            surface.missingRequirementPolicy = core::MissingRequirementPolicy::RejectPlan;
            pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(std::move(surface),
                                                                            std::move(callbacks.hybridSurface)));
        }
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(
            feature("global-illumination", {path_ == DeferredRenderPath::Raster ? "atmosphere-luts" : "hybrid-surface"},
                    {core::HistoryDomain::Sharc}),
            std::move(callbacks.globalIllumination)));
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(
            feature("gi-denoiser", {"global-illumination"},
                    {core::HistoryDomain::NrdDiffuse, core::HistoryDomain::NrdSpecular}),
            std::move(callbacks.giDenoiser)));
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(
            feature("sky-composite", {"gi-denoiser", "atmosphere-luts"}), std::move(callbacks.skyComposite)));
        core::FeatureDescriptor directLighting =
            feature("direct-lighting", {"global-illumination", "gi-denoiser", "sky-composite"});
        if (path_ == DeferredRenderPath::Raster) {
            directLighting.dependencies.emplace_back("shadow");
            directLighting.dependencies.emplace_back("gbuffer");
        }
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(std::move(directLighting),
                                                                        std::move(callbacks.directLighting)));
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(
            feature("temporal-aa", {"direct-lighting"}, {core::HistoryDomain::Taa}), std::move(callbacks.temporalAa)));
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(feature("tone-mapping", {"temporal-aa"}),
                                                                       std::move(callbacks.toneMapping)));
        pipeline_.addFeature(std::make_unique<DeferredCallbackFeature>(feature("ui-present", {"tone-mapping"}),
                                                                       std::move(callbacks.uiPresent)));
        pipeline_.resolve(capabilities);
    }

    void DeferredRenderPipeline::prepareFrame(const core::RenderFrameIdentity& identity, std::uint64_t cameraCutEpoch,
                                              const core::FrameChangeSet& changes, FrameGraph& frameGraph,
                                              core::RenderBlackboard& blackboard) {
        pipeline_.prepareFrame(identity, cameraCutEpoch, changes, frameGraph, blackboard);
    }

    void DeferredRenderPipeline::commitFrame(const core::RenderFrameIdentity& identity) {
        pipeline_.commitFrame(identity);
    }

    void DeferredRenderPipeline::discardFrame() noexcept {
        pipeline_.discardFrame();
    }

    const core::ResolvedRenderFeaturePlan& DeferredRenderPipeline::resolvedPlan() const noexcept {
        return *pipeline_.resolvedPlan();
    }

    bool DeferredRenderPipeline::hasPendingFrame() const noexcept {
        return pipeline_.hasPendingFrame();
    }

    const core::HistoryDomainState& DeferredRenderPipeline::historyState(core::HistoryDomain domain) const {
        return pipeline_.historyState(domain);
    }

    DeferredRenderPath DeferredRenderPipeline::path() const noexcept {
        return path_;
    }

} // namespace lumin::render

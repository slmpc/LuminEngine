#include "render/pipelines/default/DefaultRenderPipelineSession.hpp"

#include <functional>
#include <memory>
#include <utility>

namespace lumin::render::pipelines {
    namespace {

        struct FeatureModuleCallbacks {
            using AddPasses = std::function<void(core::RenderFeatureFrameContext&)>;
            using FrameEvent = std::function<void(const core::RenderFrameIdentity&)>;

            FeatureModuleCallbacks(AddPasses addPassesCallback, FrameEvent submittedCallback = {},
                                   FrameEvent discardedCallback = {})
                : addPasses(std::move(addPassesCallback)), submitted(std::move(submittedCallback)),
                  discarded(std::move(discardedCallback)) {
            }

            AddPasses addPasses;
            FrameEvent submitted;
            FrameEvent discarded;
        };

        class DefaultFeatureModule final : public core::IRenderFeature {
        public:
            DefaultFeatureModule(core::FeatureDescriptor descriptor, FeatureModuleCallbacks callbacks)
                : descriptor_(std::move(descriptor)), callbacks_(std::move(callbacks)) {
            }

            [[nodiscard]] const core::FeatureDescriptor& descriptor() const noexcept override {
                return descriptor_;
            }

            void addPasses(core::RenderFeatureFrameContext& context) override {
                callbacks_.addPasses(context);
            }

            void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
                if (callbacks_.submitted) {
                    callbacks_.submitted(identity);
                }
            }

            void onFrameDiscarded(const core::RenderFrameIdentity& identity) noexcept override {
                if (callbacks_.discarded) {
                    callbacks_.discarded(identity);
                }
            }

        private:
            core::FeatureDescriptor descriptor_;
            FeatureModuleCallbacks callbacks_;
        };

        void registerStaticModule(core::RenderFeatureRegistry& registry, core::FeatureDescriptor descriptor,
                                  FeatureModuleCallbacks callbacks) {
            core::FeatureDescriptor factoryDescriptor = descriptor;
            registry.registerFeature(
                std::move(descriptor),
                [descriptor = std::move(factoryDescriptor), callbacks = std::move(callbacks)](
                    const core::FeatureCreateContext&) {
                    return std::make_unique<DefaultFeatureModule>(descriptor, callbacks);
                });
        }

    } // namespace

    core::RenderFeatureRegistry DefaultRenderPipelineSession::createFeatureRegistry(
        const DefaultRenderPipelineDefinition& definition, DefaultRenderPipelineKind path) {
        core::RenderFeatureRegistry registry;
        const auto registerModule = [&registry, &definition](const core::FeatureId& id,
                                                             FeatureModuleCallbacks callbacks) {
            registerStaticModule(registry, definition.descriptor(id), std::move(callbacks));
        };

        using namespace feature_ids;
        // 每个模块显式声明提交/丢弃边界，frame replace 或录制失败不会推进任何历史候选。
        registerModule(atmosphere(), {[this](core::RenderFeatureFrameContext& context) {
                                          addAtmosphereLutFeaturePasses(context);
                                      },
                                      [this](const core::RenderFrameIdentity& identity) {
                                          commitAtmosphereFeature(identity);
                                      },
                                      [this](const core::RenderFrameIdentity&) {
                                          discardAtmosphereFeature();
                                      }});
        if (path == DefaultRenderPipelineKind::Raster) {
            registerModule(shadow(), {[this](core::RenderFeatureFrameContext& context) {
                               addShadowFeaturePasses(context);
                           }});
            registerModule(rasterSurface(), {[this](core::RenderFeatureFrameContext& context) {
                                                 addGBufferFeaturePasses(context);
                                             },
                                             [this](const core::RenderFrameIdentity&) {
                                                 if (modelRenderer_ != nullptr) {
                                                     modelRenderer_->commitSubmittedFrame();
                                                 }
                                             },
                                             [this](const core::RenderFrameIdentity&) {
                                                 if (modelRenderer_ != nullptr) {
                                                     modelRenderer_->discardPendingFrame();
                                                 }
                                             }});
        } else {
            registerModule(hybridSurface(), {[this](core::RenderFeatureFrameContext& context) {
                                                 addHybridSurfaceFeaturePasses(context);
                                             },
                                             [this](const core::RenderFrameIdentity& identity) {
                                                 commitHybridSurfaceFeature(identity);
                                                 if (modelRenderer_ != nullptr) {
                                                     modelRenderer_->commitSubmittedFrame();
                                                 }
                                             },
                                             [this](const core::RenderFrameIdentity&) {
                                                 discardHybridSurfaceFeature();
                                                 if (modelRenderer_ != nullptr) {
                                                     modelRenderer_->discardPendingFrame();
                                                 }
                                             }});
        }
        registerModule(globalIllumination(), {[this](core::RenderFeatureFrameContext& context) {
                                                  addGlobalIlluminationFeaturePasses(context);
                                              },
                                              [this](const core::RenderFrameIdentity& identity) {
                                                  commitGlobalIlluminationFeature(identity);
                                              },
                                              [this](const core::RenderFrameIdentity&) {
                                                  discardGlobalIlluminationFeature();
                                              }});
        registerModule(denoising(), {[this](core::RenderFeatureFrameContext& context) {
                                         addGiDenoiserFeaturePasses(context);
                                     },
                                     [this](const core::RenderFrameIdentity& identity) {
                                         commitGiDenoiserFeature(identity);
                                     },
                                     [this](const core::RenderFrameIdentity&) {
                                         discardGiDenoiserFeature();
                                     }});
        registerModule(lightingComposite(), {[this](core::RenderFeatureFrameContext& context) {
                           addSkyCompositeFeaturePasses(context);
                           addDirectLightingFeaturePasses(context);
                       }});
        registerModule(temporalAa(), {[this](core::RenderFeatureFrameContext& context) {
                                          addTemporalAaFeaturePasses(context);
                                      },
                                      [this](const core::RenderFrameIdentity& identity) {
                                          postFxResources_.markHistoryValid(identity.frameSlot.value());
                                      }});
        registerModule(toneMapping(), {[this](core::RenderFeatureFrameContext& context) {
                           addToneMappingFeaturePasses(context);
                       }});
        registerModule(presentation(), {[this](core::RenderFeatureFrameContext& context) {
                                            addUiPresentFeaturePasses(context);
                                        },
                                        [this](const core::RenderFrameIdentity&) {
                                            viewportOutputInitialized_ = true;
                                        }});
        return registry;
    }

} // namespace lumin::render::pipelines

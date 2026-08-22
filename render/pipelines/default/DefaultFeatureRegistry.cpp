#include "render/pipelines/default/DefaultRenderPipelineSession.hpp"

#include <memory>
#include <utility>

namespace lumin::render::pipelines {

    class DefaultRenderPipelineSession::FeatureModuleBase : public core::IRenderFeature {
    public:
        FeatureModuleBase(DefaultRenderPipelineSession& session, core::FeatureDescriptor descriptor)
            : session_(session), descriptor_(std::move(descriptor)) {
        }

        [[nodiscard]] const core::FeatureDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

    protected:
        DefaultRenderPipelineSession& session_;

    private:
        core::FeatureDescriptor descriptor_;
    };

    class DefaultRenderPipelineSession::AtmosphereFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addAtmosphereLutFeaturePasses(context);
        }

        void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
            session_.commitAtmosphereFeature(identity);
        }

        void onFrameDiscarded(const core::RenderFrameIdentity&) noexcept override {
            session_.discardAtmosphereFeature();
        }
    };

    class DefaultRenderPipelineSession::ShadowFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addShadowFeaturePasses(context);
        }
    };

    class DefaultRenderPipelineSession::RasterSurfaceFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addGBufferFeaturePasses(context);
        }

        void onFrameSubmitted(const core::RenderFrameIdentity&) noexcept override {
            if (session_.modelRenderer_ != nullptr) {
                session_.modelRenderer_->commitSubmittedFrame();
            }
        }

        void onFrameDiscarded(const core::RenderFrameIdentity&) noexcept override {
            if (session_.modelRenderer_ != nullptr) {
                session_.modelRenderer_->discardPendingFrame();
            }
        }
    };

    class DefaultRenderPipelineSession::HybridSurfaceFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addHybridSurfaceFeaturePasses(context);
        }

        void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
            session_.commitHybridSurfaceFeature(identity);
            if (session_.modelRenderer_ != nullptr) {
                session_.modelRenderer_->commitSubmittedFrame();
            }
        }

        void onFrameDiscarded(const core::RenderFrameIdentity&) noexcept override {
            session_.discardHybridSurfaceFeature();
            if (session_.modelRenderer_ != nullptr) {
                session_.modelRenderer_->discardPendingFrame();
            }
        }
    };

    class DefaultRenderPipelineSession::GlobalIlluminationFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addGlobalIlluminationFeaturePasses(context);
        }

        void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
            session_.commitGlobalIlluminationFeature(identity);
        }

        void onFrameDiscarded(const core::RenderFrameIdentity&) noexcept override {
            session_.discardGlobalIlluminationFeature();
        }
    };

    class DefaultRenderPipelineSession::DenoisingFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addGiDenoiserFeaturePasses(context);
        }

        void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
            session_.commitGiDenoiserFeature(identity);
        }

        void onFrameDiscarded(const core::RenderFrameIdentity&) noexcept override {
            session_.discardGiDenoiserFeature();
        }
    };

    class DefaultRenderPipelineSession::LightingCompositeFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addSkyCompositeFeaturePasses(context);
            session_.addDirectLightingFeaturePasses(context);
        }
    };

    class DefaultRenderPipelineSession::TemporalAaFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addTemporalAaFeaturePasses(context);
        }

        void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override {
            session_.postFxResources_.markHistoryValid(identity.frameSlot.value());
        }
    };

    class DefaultRenderPipelineSession::ToneMappingFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addToneMappingFeaturePasses(context);
        }
    };

    class DefaultRenderPipelineSession::PresentationFeatureModule final : public FeatureModuleBase {
    public:
        using FeatureModuleBase::FeatureModuleBase;

        void addPasses(core::RenderFeatureFrameContext& context) override {
            session_.addUiPresentFeaturePasses(context);
        }

        void onFrameSubmitted(const core::RenderFrameIdentity&) noexcept override {
            session_.viewportOutputInitialized_ = true;
        }
    };

    core::RenderFeatureRegistry DefaultRenderPipelineSession::createFeatureRegistry(
        const DefaultRenderPipelineDefinition& definition, DefaultRenderPipelineKind path) {
        core::RenderFeatureRegistry registry;
        const auto registerModule = [&registry, &definition, this]<typename Module>(const core::FeatureId& id) {
            core::FeatureDescriptor descriptor = definition.descriptor(id);
            core::FeatureDescriptor instanceDescriptor = descriptor;
            registry.registerFeature(
                std::move(descriptor),
                [this, descriptor = std::move(instanceDescriptor)](const core::FeatureCreateContext&) {
                    return std::make_unique<Module>(*this, descriptor);
                });
        };

        using namespace feature_ids;
        // 每个静态模块直接实现自己的提交/丢弃边界，失败帧不会通过中央回调壳推进历史。
        registerModule.template operator()<AtmosphereFeatureModule>(atmosphere());
        if (path == DefaultRenderPipelineKind::Raster) {
            registerModule.template operator()<ShadowFeatureModule>(shadow());
            registerModule.template operator()<RasterSurfaceFeatureModule>(rasterSurface());
        } else {
            registerModule.template operator()<HybridSurfaceFeatureModule>(hybridSurface());
        }
        registerModule.template operator()<GlobalIlluminationFeatureModule>(globalIllumination());
        registerModule.template operator()<DenoisingFeatureModule>(denoising());
        registerModule.template operator()<LightingCompositeFeatureModule>(lightingComposite());
        registerModule.template operator()<TemporalAaFeatureModule>(temporalAa());
        registerModule.template operator()<ToneMappingFeatureModule>(toneMapping());
        registerModule.template operator()<PresentationFeatureModule>(presentation());
        return registry;
    }

} // namespace lumin::render::pipelines

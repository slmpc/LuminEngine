#pragma once

#include "render/core/RenderFeaturePipeline.hpp"

#include <cstdint>
#include <memory>

namespace lumin::render {

    enum class DeferredRenderPath : std::uint8_t;

    enum class LevelRenderFeatureKind : std::uint8_t {
        Shadow,
        GBuffer,
        HybridSurface,
        AtmosphereLuts,
        GlobalIllumination,
        GiDenoiser,
        SkyComposite,
        DirectLighting,
        TemporalAa,
        ToneMapping,
        UiPresent,
    };

    /**
     * @brief LevelRenderer 与独立 Feature 之间的窄化宿主接口。
     *
     * Feature 只拥有 descriptor 和本身的生命周期通知；所有逐帧数据通过 blackboard 传递，
     * 不依赖 LevelRenderer 的具体类型，也不跨帧持有 RenderFeatureFrameContext。
     */
    class LevelRenderFeatureHost {
    public:
        virtual ~LevelRenderFeatureHost() = default;

        LevelRenderFeatureHost(const LevelRenderFeatureHost&) = delete;
        LevelRenderFeatureHost& operator=(const LevelRenderFeatureHost&) = delete;

        virtual void addFeaturePasses(LevelRenderFeatureKind kind, core::RenderFeatureFrameContext& context) = 0;
        virtual void submitFeature(LevelRenderFeatureKind kind,
                                   const core::RenderFrameIdentity& identity) noexcept = 0;
        virtual void discardFeature(LevelRenderFeatureKind kind,
                                    const core::RenderFrameIdentity& identity) noexcept = 0;

    protected:
        LevelRenderFeatureHost() = default;
    };

    /** 将一个 LevelRenderer pass 组适配为可独立注册的 RenderFeature。 */
    class LevelRenderFeature final : public core::IRenderFeature {
    public:
        LevelRenderFeature(LevelRenderFeatureKind kind, core::FeatureDescriptor descriptor,
                           LevelRenderFeatureHost& host) noexcept;

        [[nodiscard]] const core::FeatureDescriptor& descriptor() const noexcept override;
        void addPasses(core::RenderFeatureFrameContext& context) override;
        void onFrameSubmitted(const core::RenderFrameIdentity& identity) noexcept override;
        void onFrameDiscarded(const core::RenderFrameIdentity& identity) noexcept override;

    private:
        LevelRenderFeatureKind kind_;
        core::FeatureDescriptor descriptor_;
        LevelRenderFeatureHost& host_;
    };

    [[nodiscard]] std::unique_ptr<core::IRenderFeature>
    makeLevelRenderFeature(LevelRenderFeatureKind kind, core::FeatureDescriptor descriptor,
                           LevelRenderFeatureHost& host);

} // namespace lumin::render

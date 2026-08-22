#include "render/gi/raytracing/NrdDenoiser.hpp"

#include <NRD.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

    static_assert(noexcept(std::declval<lumin::render::gi::NrdDenoiser&>().discardFrame(
        std::declval<const lumin::render::gi::NrdPreparedFrame&>())));
    static_assert(noexcept(std::declval<lumin::render::gi::NrdDenoiser&>().discardPendingFrame()));

    using lumin::render::FrameGraph;
    using lumin::render::FrameGraphBufferDesc;
    using lumin::render::FrameGraphPassHandle;
    using lumin::render::FrameGraphResourceHandle;
    using lumin::render::FrameGraphTextureDesc;
    using lumin::render::core::HistoryAction;
    using lumin::render::core::HistoryDomain;
    using lumin::render::core::RenderExtent;
    using lumin::render::core::RenderSequence;
    using lumin::render::gi::NrdFrameParameters;
    using lumin::render::gi::NrdHistoryTracker;
    using lumin::render::gi::detail::NrdDispatchGraphBinding;
    using lumin::render::gi::detail::NrdDispatchPlan;
    using lumin::render::gi::detail::NrdPoolKind;
    using lumin::render::gi::detail::NrdResourceSource;

    [[noreturn]] void fail(const std::string& message) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }

    void require(bool condition, const std::string& message) {
        if (!condition) {
            fail(message);
        }
    }

    template <typename Exception, typename Callback>
    void requireThrows(Callback&& callback, const std::string& message) {
        try {
            std::forward<Callback>(callback)();
        } catch (const Exception&) {
            return;
        } catch (...) {
            fail(message + " (wrong exception type)");
        }
        fail(message + " (no exception)");
    }

    class NrdInstanceOwner final {
    public:
        NrdInstanceOwner() {
            const nrd::DenoiserDesc denoiser{identifier, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR};
            nrd::InstanceCreationDesc creation{};
            creation.denoisers = &denoiser;
            creation.denoisersNum = 1;
            if (nrd::CreateInstance(creation, instance_) != nrd::Result::SUCCESS || instance_ == nullptr) {
                fail("nrd::CreateInstance must create REBLUR_DIFFUSE_SPECULAR for adapter tests.");
            }
            nrd::ReblurSettings settings{};
            if (nrd::SetDenoiserSettings(*instance_, identifier, &settings) != nrd::Result::SUCCESS) {
                fail("nrd::SetDenoiserSettings must accept default REBLUR settings.");
            }
        }

        ~NrdInstanceOwner() {
            nrd::DestroyInstance(*instance_);
        }

        NrdInstanceOwner(const NrdInstanceOwner&) = delete;
        NrdInstanceOwner& operator=(const NrdInstanceOwner&) = delete;

        [[nodiscard]] nrd::Instance& get() const noexcept {
            return *instance_;
        }

        static constexpr nrd::Identifier identifier = 41;

    private:
        nrd::Instance* instance_ = nullptr;
    };

    void testFormatMapping() {
        using lumin::render::gi::detail::nrdFormatToNvrhi;
        constexpr std::array pairs = {
            std::pair{nrd::Format::R8_UNORM, nvrhi::Format::R8_UNORM},
            std::pair{nrd::Format::RG8_SNORM, nvrhi::Format::RG8_SNORM},
            std::pair{nrd::Format::RGBA8_SRGB, nvrhi::Format::SRGBA8_UNORM},
            std::pair{nrd::Format::R16_SFLOAT, nvrhi::Format::R16_FLOAT},
            std::pair{nrd::Format::RG16_SFLOAT, nvrhi::Format::RG16_FLOAT},
            std::pair{nrd::Format::RGBA16_SFLOAT, nvrhi::Format::RGBA16_FLOAT},
            std::pair{nrd::Format::R32_SFLOAT, nvrhi::Format::R32_FLOAT},
            std::pair{nrd::Format::RGBA32_UINT, nvrhi::Format::RGBA32_UINT},
            std::pair{nrd::Format::R10_G10_B10_A2_UNORM, nvrhi::Format::R10G10B10A2_UNORM},
            std::pair{nrd::Format::R11_G11_B10_UFLOAT, nvrhi::Format::R11G11B10_FLOAT},
        };
        for (const auto& [nrdFormat, nvrhiFormat] : pairs) {
            require(nrdFormatToNvrhi(nrdFormat) == nvrhiFormat,
                    "Every NRD pool format must map to the exact NvRHI format.");
        }
        requireThrows<std::invalid_argument>(
            [] {
                (void)nrdFormatToNvrhi(nrd::Format::R10_G10_B10_A2_UINT);
            },
            "Formats without NvRHI equivalents must fail instead of silently changing representation.");
        requireThrows<std::invalid_argument>(
            [] {
                (void)nrdFormatToNvrhi(nrd::Format::MAX_NUM);
            },
            "Unknown NRD formats must fail fast.");

        const nrd::LibraryDesc& library = *nrd::GetLibraryDesc();
        require(lumin::render::gi::detail::nrdNormalEncodingFormat(library.normalEncoding) != nvrhi::Format::UNKNOWN,
                "The configured normal encoding must expose a concrete adapter format.");
    }

    void testPoolAndResourceTranslation() {
        NrdInstanceOwner owner;
        const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(owner.get());
        require(desc.permanentPoolSize > 0 && desc.transientPoolSize > 0,
                "REBLUR diffuse/specular must expose permanent and transient pools.");

        const RenderExtent oddExtent{1919, 1079};
        for (std::uint32_t index = 0; index < desc.permanentPoolSize; ++index) {
            const nrd::TextureDesc& source = desc.permanentPool[index];
            const auto plan = lumin::render::gi::detail::makeNrdPoolTexturePlan(
                NrdPoolKind::Permanent, static_cast<std::uint16_t>(index), source, oddExtent, "pool");
            require(plan.pool == NrdPoolKind::Permanent && plan.index == index,
                    "Pool translation must preserve pool identity and index.");
            require(plan.texture.width == (oddExtent.width + source.downsampleFactor - 1U) / source.downsampleFactor &&
                        plan.texture.height ==
                            (oddExtent.height + source.downsampleFactor - 1U) / source.downsampleFactor,
                    "Pool dimensions must use ceil(resourceSize / downsampleFactor).");
            require(plan.texture.isShaderResource && plan.texture.isUAV &&
                        plan.texture.initialState == nvrhi::ResourceStates::Common && !plan.texture.keepInitialState,
                    "NRD pools must use manual FrameGraph state tracking with SRV/UAV support.");
        }

        const auto permanent = lumin::render::gi::detail::translateNrdResource(
            {nrd::DescriptorType::TEXTURE, nrd::ResourceType::PERMANENT_POOL, 0}, desc);
        const auto transient = lumin::render::gi::detail::translateNrdResource(
            {nrd::DescriptorType::STORAGE_TEXTURE, nrd::ResourceType::TRANSIENT_POOL, 0}, desc);
        const auto user = lumin::render::gi::detail::translateNrdResource(
            {nrd::DescriptorType::TEXTURE, nrd::ResourceType::IN_VIEWZ, 0}, desc);
        require(permanent.source == NrdResourceSource::PermanentPool && permanent.poolIndex == 0,
                "Permanent resources must resolve against the permanent pool.");
        require(transient.source == NrdResourceSource::TransientPool && transient.poolIndex == 0,
                "Transient resources must resolve against the transient pool.");
        require(user.source == NrdResourceSource::User && user.userType == nrd::ResourceType::IN_VIEWZ,
                "Application resources must preserve their ResourceType identity.");
        requireThrows<std::out_of_range>(
            [&] {
                (void)lumin::render::gi::detail::translateNrdResource(
                    {nrd::DescriptorType::TEXTURE, nrd::ResourceType::PERMANENT_POOL,
                     static_cast<std::uint16_t>(desc.permanentPoolSize)},
                    desc);
            },
            "Out-of-range pool indices must be rejected.");
    }

    void testPipelineBindingAbi() {
        NrdInstanceOwner owner;
        const nrd::LibraryDesc& library = *nrd::GetLibraryDesc();
        const nrd::InstanceDesc& instance = *nrd::GetInstanceDesc(owner.get());
        require(instance.resourcesSpaceIndex != instance.constantBufferAndSamplersSpaceIndex,
                "NRD resources and constants must occupy distinct descriptor sets.");

        for (std::uint32_t pipelineIndex = 0; pipelineIndex < instance.pipelinesNum; ++pipelineIndex) {
            const nrd::PipelineDesc& pipeline = instance.pipelines[pipelineIndex];
            const auto layouts = lumin::render::gi::detail::makeNrdPipelineBindingLayouts(library, instance, pipeline);
            require(layouts.resources.registerSpaceIsDescriptorSet &&
                        layouts.resources.registerSpace == instance.resourcesSpaceIndex,
                    "NRD resources layout must target the shader resources descriptor set.");
            require(layouts.constantsAndSamplers.registerSpaceIsDescriptorSet &&
                        layouts.constantsAndSamplers.registerSpace == instance.constantBufferAndSamplersSpaceIndex,
                    "NRD constants layout must target the constants/samplers descriptor set.");
            for (const nvrhi::BindingLayoutDesc* layout : {&layouts.resources, &layouts.constantsAndSamplers}) {
                require(layout->bindingOffsets.sampler == library.spirvBindingOffsets.samplerOffset &&
                            layout->bindingOffsets.shaderResource == library.spirvBindingOffsets.textureOffset &&
                            layout->bindingOffsets.constantBuffer == library.spirvBindingOffsets.constantBufferOffset &&
                            layout->bindingOffsets.unorderedAccess ==
                                library.spirvBindingOffsets.storageTextureAndBufferOffset,
                        "NvRHI binding offsets must exactly match LibraryDesc::spirvBindingOffsets.");
            }
            require(!layouts.constantsAndSamplers.bindings.empty() &&
                        layouts.constantsAndSamplers.bindings.front() ==
                            nvrhi::BindingLayoutItem::VolatileConstantBuffer(instance.constantBufferRegisterIndex),
                    "NRD constants must use a volatile per-frame-slot constant buffer.");
            require(layouts.constantsAndSamplers.bindings.size() == instance.samplersNum + 1,
                    "The shared layout must bind every NRD sampler.");

            std::size_t expectedResourceBindings = 0;
            for (std::uint32_t rangeIndex = 0; rangeIndex < pipeline.resourceRangesNum; ++rangeIndex) {
                expectedResourceBindings += pipeline.resourceRanges[rangeIndex].descriptorsNum;
            }
            require(layouts.resources.bindings.size() == expectedResourceBindings,
                    "Tight pipeline layouts must have one item per independent SPIR-V binding.");
            for (const nvrhi::BindingLayoutItem& item : layouts.resources.bindings) {
                require(item.size == 1, "NRD tN/uN bindings must not be translated into descriptor arrays.");
            }
        }
    }

    void testDispatchPlanningAndCommonSettings() {
        NrdInstanceOwner owner;
        NrdHistoryTracker history;
        const auto historyPlan =
            history.beginFrame(RenderSequence{7}, HistoryAction::Keep, HistoryAction::Keep, false, false);
        NrdFrameParameters parameters;
        parameters.sequence = RenderSequence{7};
        parameters.extent = {1280, 720};
        parameters.camera.jitter = {0.25F, -0.25F};
        parameters.camera.jitterPrevious = {-0.25F, 0.25F};
        const nrd::CommonSettings common = lumin::render::gi::detail::makeNrdCommonSettings(parameters, historyPlan);
        require(common.frameIndex == 0 && common.accumulationMode == nrd::AccumulationMode::CLEAR_AND_RESTART,
                "First use must clear pools without advancing committed frameIndex.");
        require(common.motionVectorScale[0] == 1.0F && common.motionVectorScale[1] == 1.0F &&
                    common.motionVectorScale[2] == 0.0F && !common.isMotionVectorInWorldSpace,
                "CommonSettings must consume previous-current motion directly in screen UV.");
        require(common.cameraJitter[0] == 0.25F && common.cameraJitterPrev[0] == -0.25F,
                "Current and previous pixel jitter must be copied explicitly.");

        require(nrd::SetCommonSettings(owner.get(), common) == nrd::Result::SUCCESS,
                "Generated CommonSettings must be accepted by NRD.");
        const nrd::DispatchDesc* dispatches = nullptr;
        std::uint32_t dispatchCount = 0;
        require(nrd::GetComputeDispatches(owner.get(), &NrdInstanceOwner::identifier, 1, dispatches, dispatchCount) ==
                        nrd::Result::SUCCESS &&
                    dispatchCount > 0,
                "REBLUR must return a non-empty dispatch list.");
        const nrd::InstanceDesc& instance = *nrd::GetInstanceDesc(owner.get());
        const auto plans = lumin::render::gi::detail::makeNrdDispatchPlans(
            instance, std::span<const nrd::DispatchDesc>(dispatches, dispatchCount));
        require(plans.size() == dispatchCount, "Dispatch planning must deep-copy every NRD dispatch.");

        std::set<nrd::ResourceType> userResources;
        for (const auto& plan : plans) {
            require(!plan.name.empty() && plan.pipelineIndex < instance.pipelinesNum && plan.gridWidth > 0 &&
                        plan.gridHeight > 0,
                    "Every dispatch plan must retain a valid pipeline, name, and grid.");
            for (const auto& resource : plan.resources) {
                if (resource.resource.source == NrdResourceSource::User) {
                    userResources.insert(resource.resource.userType);
                }
            }
        }
        for (const nrd::ResourceType required : {
                 nrd::ResourceType::IN_MV,
                 nrd::ResourceType::IN_NORMAL_ROUGHNESS,
                 nrd::ResourceType::IN_VIEWZ,
                 nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST,
                 nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST,
                 nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST,
                 nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST,
             }) {
            require(userResources.contains(required),
                    "Dispatch translation must preserve every required REBLUR user resource.");
        }
        history.discardFrame(RenderSequence{7});
    }

    void testVolatileConstantUploadPlanning() {
        std::vector<NrdDispatchPlan> dispatches(4);
        dispatches[1].constantData.assign(32, 0x2a);
        dispatches[2].constantData.assign(32, 0x7f);
        dispatches[2].constantDataMatchesPreviousDispatch = true;

        const auto uploads = lumin::render::gi::detail::makeNrdConstantUploads(dispatches);
        require(uploads.size() == dispatches.size() && uploads[0].size() == 16 &&
                    std::ranges::all_of(uploads[0],
                                        [](std::uint8_t value) {
                                            return value == 0;
                                        }),
                "The first dispatch must create a zeroed volatile constant-buffer version when NRD has no data.");
        require(uploads[1] == dispatches[1].constantData,
                "A dispatch with new constants must create a new volatile buffer version.");
        require(uploads[2].empty(),
                "A dispatch matching the previous constants must reuse the current command-list version.");
        require(uploads[3].empty(), "A later dispatch without constants must not consume another version.");

        NrdDispatchPlan firstWithConstants;
        firstWithConstants.constantData.assign(16, 0x55);
        firstWithConstants.constantDataMatchesPreviousDispatch = true;
        const std::array firstOnly = {firstWithConstants};
        const auto firstUpload = lumin::render::gi::detail::makeNrdConstantUploads(firstOnly);
        require(firstUpload.size() == 1 && firstUpload[0] == firstWithConstants.constantData,
                "The first dispatch must upload its data even if NRD marks it as matching a prior dispatch.");
    }

    void testHistoryTransaction() {
        NrdHistoryTracker history;
        const auto first =
            history.beginFrame(RenderSequence{1}, HistoryAction::Keep, HistoryAction::Keep, false, false);
        require(first.frameIndex == 0 && first.accumulationMode == nrd::AccumulationMode::CLEAR_AND_RESTART &&
                    first.effectiveDiffuseAction == HistoryAction::FullReset &&
                    first.effectiveSpecularAction == HistoryAction::FullReset,
                "Uninitialized diffuse/specular history must clear and restart together.");
        history.discardFrame(RenderSequence{1});
        require(history.state(HistoryDomain::NrdDiffuse).acceptedFrameCount == 0 &&
                    history.state(HistoryDomain::NrdSpecular).acceptedFrameCount == 0,
                "Discard must not advance either NRD history domain.");

        const auto retry =
            history.beginFrame(RenderSequence{1}, HistoryAction::Keep, HistoryAction::Keep, false, false);
        require(retry.frameIndex == 0, "A discarded logical frame must reuse the committed NRD frameIndex.");
        history.commitSubmittedFrame(RenderSequence{1});
        require(history.state(HistoryDomain::NrdDiffuse).acceptedFrameCount == 1 &&
                    history.state(HistoryDomain::NrdDiffuse).resetEpoch == 1 &&
                    history.state(HistoryDomain::NrdSpecular).acceptedFrameCount == 1 &&
                    history.state(HistoryDomain::NrdSpecular).resetEpoch == 1,
                "Commit must atomically publish both histories.");

        const auto stable =
            history.beginFrame(RenderSequence{2}, HistoryAction::Keep, HistoryAction::Keep, false, false);
        require(stable.frameIndex == 1 && stable.accumulationMode == nrd::AccumulationMode::CONTINUE,
                "Stable submitted history must continue with the next successful frame index.");
        history.discardFrame(RenderSequence{2});

        const auto oneDomainReset =
            history.beginFrame(RenderSequence{2}, HistoryAction::SoftReset, HistoryAction::Keep, false, false);
        require(oneDomainReset.accumulationMode == nrd::AccumulationMode::RESTART &&
                    oneDomainReset.effectiveDiffuseAction == HistoryAction::FullReset &&
                    oneDomainReset.effectiveSpecularAction == HistoryAction::FullReset,
                "Combined REBLUR must propagate either domain's restart to both physical histories.");
        history.commitSubmittedFrame(RenderSequence{2});
        require(history.state(HistoryDomain::NrdDiffuse).resetEpoch == 2 &&
                    history.state(HistoryDomain::NrdSpecular).resetEpoch == 2,
                "The combined restart must advance both reset epochs only after submit.");
        requireThrows<std::invalid_argument>(
            [&] {
                (void)history.state(HistoryDomain::Taa);
            },
            "NRD tracker must reject history domains it does not own.");
    }

    void testFrameGraphDispatchTopology() {
        FrameGraph graph;
        const FrameGraphResourceHandle input = graph.importTexture("input", FrameGraphTextureDesc{});
        const FrameGraphResourceHandle intermediate = graph.importTexture("intermediate", FrameGraphTextureDesc{});
        const FrameGraphResourceHandle independent = graph.importTexture("independent", FrameGraphTextureDesc{});
        const FrameGraphResourceHandle output = graph.importTexture("output", FrameGraphTextureDesc{});
        const FrameGraphResourceHandle constants = graph.importBuffer("constants", FrameGraphBufferDesc{});
        std::vector<int> execution;

        const std::array firstResources = {
            NrdDispatchGraphBinding{input, nrd::DescriptorType::TEXTURE},
            NrdDispatchGraphBinding{intermediate, nrd::DescriptorType::STORAGE_TEXTURE},
        };
        const FrameGraphPassHandle first = lumin::render::gi::detail::addNrdDispatchPass(
            graph, "nrd.first", firstResources, constants, true, {}, [&](const lumin::render::FrameGraphContext&) {
                execution.push_back(1);
            });
        const std::array secondResources = {
            NrdDispatchGraphBinding{intermediate, nrd::DescriptorType::TEXTURE},
            NrdDispatchGraphBinding{output, nrd::DescriptorType::STORAGE_TEXTURE},
        };
        const FrameGraphPassHandle second =
            lumin::render::gi::detail::addNrdDispatchPass(graph, "nrd.second", secondResources, constants, false, first,
                                                          [&](const lumin::render::FrameGraphContext&) {
                                                              execution.push_back(2);
                                                          });
        const std::array thirdResources = {
            NrdDispatchGraphBinding{independent, nrd::DescriptorType::TEXTURE},
        };
        const FrameGraphPassHandle third = lumin::render::gi::detail::addNrdDispatchPass(
            graph, "nrd.third", thirdResources, constants, false, second, [&](const lumin::render::FrameGraphContext&) {
                execution.push_back(3);
            });

        graph.compile();
        const std::array expectedOrder = {first.id, second.id, third.id};
        require(std::ranges::equal(graph.executionOrder(), expectedOrder),
                "NRD dispatches must remain in API order even when a pass has no shared texture hazard.");
        graph.execute({});
        require(execution == std::vector<int>{1, 2, 3},
                "Compiled NRD FrameGraph topology must execute every dispatch exactly once in order.");
    }

} // namespace

int main() {
    try {
        testFormatMapping();
        testPoolAndResourceTranslation();
        testPipelineBindingAbi();
        testDispatchPlanningAndCommonSettings();
        testVolatileConstantUploadPlanning();
        testHistoryTransaction();
        testFrameGraphDispatchTopology();
    } catch (const std::exception& error) {
        fail(std::string("Unexpected exception: ") + error.what());
    }
    std::puts("PASS: NRD format/pool/dispatch translation, history transaction, and FrameGraph topology.");
    return 0;
}

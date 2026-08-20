#include "render/resources/TextureManager.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    using lumin::render::TextureFrameResources;
    using lumin::render::TextureManager;

    static_assert(std::same_as<decltype(TextureFrameResources::position), lumin::render::GpuTexture>);
    static_assert(std::same_as<decltype(TextureFrameResources::postProcessUniform), lumin::render::GpuBuffer>);
    static_assert(std::same_as<decltype(std::declval<const TextureManager&>().positionFormat()), nvrhi::Format>);
    static_assert(std::same_as<decltype(std::declval<const TextureManager&>().sampler()), nvrhi::SamplerHandle>);
    static_assert(
        std::same_as<decltype(std::declval<const TextureManager&>().bindingLayout()), nvrhi::BindingLayoutHandle>);
    static_assert(std::same_as<decltype(std::declval<const TextureManager&>().bindingSet(0)), nvrhi::BindingSetHandle>);

    struct LiveCounts {
        int textures = 0;
        int buffers = 0;
        int samplers = 0;
        int layouts = 0;
        int bindingSets = 0;
        std::vector<std::string> releases;
    };

    class FakeTexture final : public nvrhi::RefCounter<nvrhi::ITexture> {
    public:
        FakeTexture(nvrhi::TextureDesc desc, LiveCounts& counts) : desc_(std::move(desc)), counts_(counts) {
            ++counts_.textures;
        }

        ~FakeTexture() override {
            counts_.releases.emplace_back("texture");
            --counts_.textures;
        }

        [[nodiscard]] const nvrhi::TextureDesc& getDesc() const override {
            return desc_;
        }

        nvrhi::Object getNativeView(nvrhi::ObjectType, nvrhi::Format, nvrhi::TextureSubresourceSet,
                                    nvrhi::TextureDimension, bool) override {
            return nullptr;
        }

    private:
        nvrhi::TextureDesc desc_;
        LiveCounts& counts_;
    };

    class FakeBuffer final : public nvrhi::RefCounter<nvrhi::IBuffer> {
    public:
        FakeBuffer(nvrhi::BufferDesc desc, LiveCounts& counts)
            : desc_(std::move(desc)), bytes_(static_cast<std::size_t>(desc_.byteSize)), counts_(counts) {
            ++counts_.buffers;
        }

        ~FakeBuffer() override {
            counts_.releases.emplace_back("buffer");
            --counts_.buffers;
        }

        [[nodiscard]] const nvrhi::BufferDesc& getDesc() const override {
            return desc_;
        }

        [[nodiscard]] nvrhi::GpuVirtualAddress getGpuVirtualAddress() const override {
            return 0;
        }

        [[nodiscard]] void* data() noexcept {
            return bytes_.data();
        }

    private:
        nvrhi::BufferDesc desc_;
        std::vector<std::byte> bytes_;
        LiveCounts& counts_;
    };

    class FakeSampler final : public nvrhi::RefCounter<nvrhi::ISampler> {
    public:
        FakeSampler(nvrhi::SamplerDesc desc, LiveCounts& counts) : desc_(desc), counts_(counts) {
            ++counts_.samplers;
        }

        ~FakeSampler() override {
            counts_.releases.emplace_back("sampler");
            --counts_.samplers;
        }

        [[nodiscard]] const nvrhi::SamplerDesc& getDesc() const override {
            return desc_;
        }

    private:
        nvrhi::SamplerDesc desc_;
        LiveCounts& counts_;
    };

    class FakeBindingLayout final : public nvrhi::RefCounter<nvrhi::IBindingLayout> {
    public:
        FakeBindingLayout(nvrhi::BindingLayoutDesc desc, LiveCounts& counts) : desc_(std::move(desc)), counts_(counts) {
            ++counts_.layouts;
        }

        ~FakeBindingLayout() override {
            counts_.releases.emplace_back("layout");
            --counts_.layouts;
        }

        [[nodiscard]] const nvrhi::BindingLayoutDesc* getDesc() const override {
            return &desc_;
        }

        [[nodiscard]] const nvrhi::BindlessLayoutDesc* getBindlessDesc() const override {
            return nullptr;
        }

    private:
        nvrhi::BindingLayoutDesc desc_;
        LiveCounts& counts_;
    };

    class FakeBindingSet final : public nvrhi::RefCounter<nvrhi::IBindingSet> {
    public:
        FakeBindingSet(nvrhi::BindingSetDesc desc, nvrhi::IBindingLayout* layout, LiveCounts& counts)
            : desc_(std::move(desc)), layout_(layout), counts_(counts) {
            ++counts_.bindingSets;
        }

        ~FakeBindingSet() override {
            counts_.releases.emplace_back("bindingSet");
            --counts_.bindingSets;
        }

        [[nodiscard]] const nvrhi::BindingSetDesc* getDesc() const override {
            return &desc_;
        }

        [[nodiscard]] nvrhi::IBindingLayout* getLayout() const override {
            return layout_;
        }

    private:
        nvrhi::BindingSetDesc desc_;
        nvrhi::IBindingLayout* layout_;
        LiveCounts& counts_;
    };

    class FakeDevice final : public nvrhi::RefCounter<nvrhi::IDevice> {
    public:
        LiveCounts live;
        std::vector<nvrhi::TextureDesc> textureDescs;
        std::vector<nvrhi::BindingSetDesc> bindingSetDescs;
        std::uint32_t failBindingSetCall = 0;

        nvrhi::HeapHandle createHeap(const nvrhi::HeapDesc&) override {
            return nullptr;
        }
        nvrhi::TextureHandle createTexture(const nvrhi::TextureDesc& desc) override {
            textureDescs.push_back(desc);
            return nvrhi::TextureHandle::Create(new FakeTexture(desc, live));
        }
        nvrhi::MemoryRequirements getTextureMemoryRequirements(nvrhi::ITexture*) override {
            return {};
        }
        bool bindTextureMemory(nvrhi::ITexture*, nvrhi::IHeap*, std::uint64_t) override {
            return false;
        }
        nvrhi::TextureHandle createHandleForNativeTexture(nvrhi::ObjectType, nvrhi::Object,
                                                          const nvrhi::TextureDesc&) override {
            return nullptr;
        }
        nvrhi::StagingTextureHandle createStagingTexture(const nvrhi::TextureDesc&, nvrhi::CpuAccessMode) override {
            return nullptr;
        }
        void* mapStagingTexture(nvrhi::IStagingTexture*, const nvrhi::TextureSlice&, nvrhi::CpuAccessMode,
                                std::size_t*) override {
            return nullptr;
        }
        void unmapStagingTexture(nvrhi::IStagingTexture*) override {
        }
        void getTextureTiling(nvrhi::ITexture*, std::uint32_t*, nvrhi::PackedMipDesc*, nvrhi::TileShape*,
                              std::uint32_t*, nvrhi::SubresourceTiling*) override {
        }
        void updateTextureTileMappings(nvrhi::ITexture*, const nvrhi::TextureTilesMapping*, std::uint32_t,
                                       nvrhi::CommandQueue) override {
        }
        nvrhi::SamplerFeedbackTextureHandle
        createSamplerFeedbackTexture(nvrhi::ITexture*, const nvrhi::SamplerFeedbackTextureDesc&) override {
            return nullptr;
        }
        nvrhi::SamplerFeedbackTextureHandle createSamplerFeedbackForNativeTexture(nvrhi::ObjectType, nvrhi::Object,
                                                                                  nvrhi::ITexture*) override {
            return nullptr;
        }
        nvrhi::BufferHandle createBuffer(const nvrhi::BufferDesc& desc) override {
            return nvrhi::BufferHandle::Create(new FakeBuffer(desc, live));
        }
        void* mapBuffer(nvrhi::IBuffer* buffer, nvrhi::CpuAccessMode) override {
            return static_cast<FakeBuffer*>(buffer)->data();
        }
        void unmapBuffer(nvrhi::IBuffer*) override {
        }
        nvrhi::MemoryRequirements getBufferMemoryRequirements(nvrhi::IBuffer*) override {
            return {};
        }
        bool bindBufferMemory(nvrhi::IBuffer*, nvrhi::IHeap*, std::uint64_t) override {
            return false;
        }
        nvrhi::BufferHandle createHandleForNativeBuffer(nvrhi::ObjectType, nvrhi::Object,
                                                        const nvrhi::BufferDesc&) override {
            return nullptr;
        }
        nvrhi::ShaderHandle createShader(const nvrhi::ShaderDesc&, const void*, std::size_t) override {
            return nullptr;
        }
        nvrhi::ShaderHandle createShaderSpecialization(nvrhi::IShader*, const nvrhi::ShaderSpecialization*,
                                                       std::uint32_t) override {
            return nullptr;
        }
        nvrhi::ShaderLibraryHandle createShaderLibrary(const void*, std::size_t) override {
            return nullptr;
        }
        nvrhi::SamplerHandle createSampler(const nvrhi::SamplerDesc& desc) override {
            return nvrhi::SamplerHandle::Create(new FakeSampler(desc, live));
        }
        nvrhi::InputLayoutHandle createInputLayout(const nvrhi::VertexAttributeDesc*, std::uint32_t,
                                                   nvrhi::IShader*) override {
            return nullptr;
        }
        nvrhi::EventQueryHandle createEventQuery() override {
            return nullptr;
        }
        void setEventQuery(nvrhi::IEventQuery*, nvrhi::CommandQueue) override {
        }
        bool pollEventQuery(nvrhi::IEventQuery*) override {
            return false;
        }
        void waitEventQuery(nvrhi::IEventQuery*) override {
        }
        void resetEventQuery(nvrhi::IEventQuery*) override {
        }
        nvrhi::TimerQueryHandle createTimerQuery() override {
            return nullptr;
        }
        bool pollTimerQuery(nvrhi::ITimerQuery*) override {
            return false;
        }
        float getTimerQueryTime(nvrhi::ITimerQuery*) override {
            return 0.0f;
        }
        void resetTimerQuery(nvrhi::ITimerQuery*) override {
        }
        nvrhi::GraphicsAPI getGraphicsAPI() override {
            return nvrhi::GraphicsAPI::VULKAN;
        }
        nvrhi::FramebufferHandle createFramebuffer(const nvrhi::FramebufferDesc&) override {
            return nullptr;
        }
        nvrhi::GraphicsPipelineHandle createGraphicsPipeline(const nvrhi::GraphicsPipelineDesc&,
                                                             const nvrhi::FramebufferInfo&) override {
            return nullptr;
        }
        nvrhi::GraphicsPipelineHandle createGraphicsPipeline(const nvrhi::GraphicsPipelineDesc&,
                                                             nvrhi::IFramebuffer*) override {
            return nullptr;
        }
        nvrhi::ComputePipelineHandle createComputePipeline(const nvrhi::ComputePipelineDesc&) override {
            return nullptr;
        }
        nvrhi::MeshletPipelineHandle createMeshletPipeline(const nvrhi::MeshletPipelineDesc&,
                                                           const nvrhi::FramebufferInfo&) override {
            return nullptr;
        }
        nvrhi::MeshletPipelineHandle createMeshletPipeline(const nvrhi::MeshletPipelineDesc&,
                                                           nvrhi::IFramebuffer*) override {
            return nullptr;
        }
        nvrhi::rt::PipelineHandle createRayTracingPipeline(const nvrhi::rt::PipelineDesc&) override {
            return nullptr;
        }
        nvrhi::BindingLayoutHandle createBindingLayout(const nvrhi::BindingLayoutDesc& desc) override {
            return nvrhi::BindingLayoutHandle::Create(new FakeBindingLayout(desc, live));
        }
        nvrhi::BindingLayoutHandle createBindlessLayout(const nvrhi::BindlessLayoutDesc&) override {
            return nullptr;
        }
        nvrhi::BindingSetHandle createBindingSet(const nvrhi::BindingSetDesc& desc,
                                                 nvrhi::IBindingLayout* layout) override {
            bindingSetDescs.push_back(desc);
            if (failBindingSetCall != 0 && bindingSetDescs.size() == failBindingSetCall) {
                return nullptr;
            }
            return nvrhi::BindingSetHandle::Create(new FakeBindingSet(desc, layout, live));
        }
        nvrhi::DescriptorTableHandle createDescriptorTable(nvrhi::IBindingLayout*) override {
            return nullptr;
        }
        void resizeDescriptorTable(nvrhi::IDescriptorTable*, std::uint32_t, bool) override {
        }
        bool writeDescriptorTable(nvrhi::IDescriptorTable*, const nvrhi::BindingSetItem&) override {
            return false;
        }
        nvrhi::rt::OpacityMicromapHandle createOpacityMicromap(const nvrhi::rt::OpacityMicromapDesc&) override {
            return nullptr;
        }
        nvrhi::rt::AccelStructHandle createAccelStruct(const nvrhi::rt::AccelStructDesc&) override {
            return nullptr;
        }
        nvrhi::MemoryRequirements getAccelStructMemoryRequirements(nvrhi::rt::IAccelStruct*) override {
            return {};
        }
        nvrhi::rt::cluster::OperationSizeInfo
        getClusterOperationSizeInfo(const nvrhi::rt::cluster::OperationParams&) override {
            return {};
        }
        bool bindAccelStructMemory(nvrhi::rt::IAccelStruct*, nvrhi::IHeap*, std::uint64_t) override {
            return false;
        }
        nvrhi::CommandListHandle createCommandList(const nvrhi::CommandListParameters&) override {
            return nullptr;
        }
        std::uint64_t executeCommandLists(nvrhi::ICommandList* const*, std::size_t, nvrhi::CommandQueue) override {
            return 0;
        }
        void queueWaitForCommandList(nvrhi::CommandQueue, nvrhi::CommandQueue, std::uint64_t) override {
        }
        bool waitForIdle() override {
            return true;
        }
        void runGarbageCollection() override {
        }
        bool queryFeatureSupport(nvrhi::Feature, void*, std::size_t) override {
            return false;
        }
        nvrhi::FormatSupport queryFormatSupport(nvrhi::Format) override {
            return nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::DepthStencil |
                   nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::ShaderLoad |
                   nvrhi::FormatSupport::ShaderSample | nvrhi::FormatSupport::ShaderUavLoad |
                   nvrhi::FormatSupport::ShaderUavStore;
        }
        nvrhi::coopvec::DeviceFeatures queryCoopVecFeatures() override {
            return {};
        }
        std::size_t getCoopVecMatrixSize(nvrhi::coopvec::DataType, nvrhi::coopvec::MatrixLayout, int, int) override {
            return 0;
        }
        nvrhi::Object getNativeQueue(nvrhi::ObjectType, nvrhi::CommandQueue) override {
            return nullptr;
        }
        nvrhi::IMessageCallback* getMessageCallback() override {
            return nullptr;
        }
        bool isAftermathEnabled() override {
            return false;
        }
        nvrhi::AftermathCrashDumpHelper& getAftermathCrashDumpHelper() override {
            std::abort();
        }
    };

    void require(bool condition, const char* message) {
        if (!condition) {
            std::fputs(message, stderr);
            std::fputc('\n', stderr);
            std::exit(1);
        }
    }

    std::string readSource(const std::filesystem::path& relativePath) {
        const std::filesystem::path root = LUMIN_TEST_SOURCE_DIR;
        std::ifstream input(root / relativePath, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    }

    void requireNoLiveHandles(const FakeDevice& device) {
        require(device.live.textures == 0, "Texture handles leaked after rollback or destroy.");
        require(device.live.buffers == 0, "Buffer handles leaked after rollback or destroy.");
        require(device.live.samplers == 0, "Sampler handles leaked after rollback or destroy.");
        require(device.live.layouts == 0, "Binding layout handles leaked after rollback or destroy.");
        require(device.live.bindingSets == 0, "Binding set handles leaked after rollback or destroy.");
    }

    const nvrhi::TextureDesc& desc(const lumin::render::GpuTexture& texture) {
        return texture.texture->getDesc();
    }

    void verifyTwoSlotContract() {
        FakeDevice device;
        TextureManager manager(device);
        manager.create(64, 32);

        require(device.live.textures == 28, "Two frame slots must own fourteen textures each.");
        require(device.live.buffers == 2, "Two frame slots must own independent uniform buffers.");
        require(device.live.bindingSets == 2, "Two frame slots must own independent binding sets.");
        require(manager.bindingSet(0) != manager.bindingSet(1), "Frame-slot binding sets must be distinct.");
        require(lumin::render::shadowCascadeCount == 4, "The CSM cascade count must remain four.");
        require(manager.positionFormat() == nvrhi::Format::RGBA16_FLOAT &&
                    manager.normalFormat() == nvrhi::Format::RGBA16_FLOAT &&
                    manager.albedoFormat() == nvrhi::Format::RGBA8_UNORM &&
                    manager.motionFormat() == nvrhi::Format::RG16_FLOAT &&
                    manager.materialIdFormat() == nvrhi::Format::R32_UINT &&
                    manager.depthFormat() == nvrhi::Format::D32 &&
                    manager.globalIlluminationFormat() == nvrhi::Format::RGBA16_FLOAT &&
                    manager.lightingFormat() == nvrhi::Format::RGBA16_FLOAT &&
                    manager.shadowDepthFormat() == nvrhi::Format::D32,
                "Texture formats must preserve the legacy preferred-format contract.");

        const auto* layout = manager.bindingLayout()->getDesc();
        require(layout != nullptr && layout->bindings.size() == 14, "Fullscreen layout must expose bindings 0-13.");
        for (std::uint32_t binding = 0; binding < 14; ++binding) {
            require(layout->bindings[binding].slot == binding,
                    "Fullscreen binding numbers must remain contiguous 0-13.");
            const nvrhi::ResourceType expected = binding < 12    ? nvrhi::ResourceType::Texture_SRV
                                                 : binding == 12 ? nvrhi::ResourceType::Sampler
                                                                 : nvrhi::ResourceType::ConstantBuffer;
            require(layout->bindings[binding].type == expected, "Fullscreen binding types must preserve 0-13.");
        }

        for (std::uint32_t frameIndex = 0; frameIndex < TextureManager::maxFramesInFlight; ++frameIndex) {
            const TextureFrameResources& frame = manager.frame(frameIndex);
            require(frame.position.texture && frame.history.texture && frame.postProcessUniform.buffer,
                    "Every frame slot must expose live NvRHI resources.");
            require(frame.position.width == 64 && frame.position.height == 32,
                    "Render textures must preserve the requested extent.");
            require(frame.shadowCascades.size() == 4, "Every frame slot must own four shadow cascades.");
            require(desc(frame.position).isRenderTarget && desc(frame.position).isShaderResource &&
                        desc(frame.position).isUAV,
                    "Surface position must support render-target, sampled, and RT UAV usage.");
            require(desc(frame.normalRoughness).isRenderTarget && desc(frame.normalRoughness).isShaderResource &&
                        desc(frame.normalRoughness).isUAV &&
                        desc(frame.albedo).isRenderTarget && desc(frame.albedo).isShaderResource &&
                        desc(frame.albedo).isUAV &&
                        desc(frame.motion).isRenderTarget && desc(frame.motion).isShaderResource &&
                        desc(frame.motion).isUAV &&
                        desc(frame.materialId).isRenderTarget && desc(frame.materialId).isShaderResource &&
                        desc(frame.materialId).isUAV &&
                        desc(frame.materialId).format == nvrhi::Format::R32_UINT,
                    "Surface color resources must support render-target, sampled, and RT UAV usage.");
            require(desc(frame.depth).isRenderTarget && !desc(frame.depth).isShaderResource && !desc(frame.depth).isUAV,
                    "G-buffer depth must remain depth-attachment-only usage.");
            require(desc(frame.globalIllumination).isRenderTarget && desc(frame.globalIllumination).isShaderResource &&
                        desc(frame.globalIllumination).isUAV,
                    "GI must remain render-target, sampled, and storage usage.");
            require(desc(frame.lighting).isRenderTarget && desc(frame.lighting).isShaderResource &&
                        desc(frame.lighting).isUAV && desc(frame.taaResolved).isRenderTarget &&
                        desc(frame.taaResolved).isShaderResource && desc(frame.taaResolved).isUAV,
                    "Lighting and TAA resolved textures must support Hybrid UAV usage.");
            require(!desc(frame.history).isRenderTarget && desc(frame.history).isShaderResource &&
                        !desc(frame.history).isUAV,
                    "TAA history must remain sampled and copy-destination compatible without render-target usage.");
            for (const lumin::render::GpuTexture& shadow : frame.shadowCascades) {
                require(desc(shadow).width == lumin::render::shadowMapResolution &&
                            desc(shadow).height == lumin::render::shadowMapResolution && desc(shadow).isRenderTarget &&
                            desc(shadow).isShaderResource,
                        "Every shadow cascade must remain a 2048-square sampled depth target.");
            }
            require(frame.postProcessUniform.buffer->getDesc().isConstantBuffer &&
                        frame.postProcessUniform.buffer->getDesc().cpuAccess == nvrhi::CpuAccessMode::Write,
                    "Post-process uniforms must remain CPU-writable constant buffers.");
            require(frame.history.initialState == nvrhi::ResourceStates::Common,
                    "History textures must begin in NvRHI's concrete first-use state.");
            require(manager.historyInitialState(frameIndex) == nvrhi::ResourceStates::Common,
                    "Uninitialized history imports must use NvRHI's supported Common/Undefined source state.");

            const nvrhi::BindingSetDesc* setDesc = manager.bindingSet(frameIndex)->getDesc();
            require(setDesc != nullptr && setDesc->bindings.size() == 14,
                    "Every frame binding set must contain bindings 0-13.");
            for (std::uint32_t binding = 0; binding < 14; ++binding) {
                require(setDesc->bindings[binding].slot == binding,
                        "Every frame binding set must preserve shader binding numbers 0-13.");
            }
            const std::uint32_t previousIndex =
                (frameIndex + TextureManager::maxFramesInFlight - 1) % TextureManager::maxFramesInFlight;
            require(setDesc->bindings[6].resourceHandle == manager.frame(previousIndex).history.texture.Get(),
                    "Binding 6 must sample the previous frame slot's history texture.");
        }

        manager.markHistoryValid(0);
        require(manager.historyValid(0) && manager.historyInitialized(0),
                "markHistoryValid must set valid and initialized together.");
        require(manager.historyInitialState(0) == nvrhi::ResourceStates::ShaderResource,
                "Written history must import as ShaderResource.");
        manager.invalidateHistory();
        require(!manager.historyValid(0) && manager.historyInitialized(0),
                "invalidateHistory must preserve initialized history storage.");

        manager.destroy();
        require(!manager.historyValid(0) && !manager.historyInitialized(0),
                "destroy must clear valid and initialized history state.");
        requireNoLiveHandles(device);
        const auto firstTexture = std::find(device.live.releases.begin(), device.live.releases.end(), "texture");
        const auto lastBindingSet = std::find(device.live.releases.rbegin(), device.live.releases.rend(), "bindingSet");
        require(firstTexture != device.live.releases.end() && lastBindingSet != device.live.releases.rend() &&
                    firstTexture > lastBindingSet.base() - 1,
                "Binding sets must release before textures.");

        manager.create(16, 16);
        require(!manager.historyValid(0) && !manager.historyInitialized(0),
                "create must clear valid and initialized history state.");
        manager.destroy();
        requireNoLiveHandles(device);
    }

    void verifyFailureRollback() {
        FakeDevice device;
        device.failBindingSetCall = 2;
        TextureManager manager(device);
        bool failed = false;
        try {
            manager.create(64, 32);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "Second-slot binding-set failure must propagate.");
        requireNoLiveHandles(device);
        manager.destroy();
        requireNoLiveHandles(device);
    }

    void verifyMalformedExtent() {
        FakeDevice device;
        TextureManager manager(device);
        bool failed = false;
        try {
            manager.create(0, 32);
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        require(failed, "TextureManager must reject an empty render extent.");
        requireNoLiveHandles(device);
    }

    void verifyForbiddenTokens() {
        const std::string production =
            readSource("render/resources/TextureManager.hpp") + readSource("render/resources/TextureManager.cpp");
        constexpr const char* forbidden[] = {"VkDescriptor",    "VkSampler",     "VkImage",   "VkImageView",
                                             "vkCreate",        "vkUpdate",      "vkDestroy", "beginTracking",
                                             "setTextureState", "commitBarriers"};
        for (const char* token : forbidden) {
            require(production.find(token) == std::string::npos, token);
        }
    }

} // namespace

int main() {
    verifyTwoSlotContract();
    verifyFailureRollback();
    verifyMalformedExtent();
    verifyForbiddenTokens();
    std::puts("PASS: two independent frame slots expose NvRHI resources, CSM=4, and bindings 0-13.");
    std::puts("PASS: history Unknown/ShaderResource states and invalidate semantics are preserved.");
    std::puts("PASS: second-slot binding failure rolls back to zero live handles.");
    std::puts("PASS: empty extents are rejected with zero live handles.");
    return 0;
}

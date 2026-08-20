#include "render/resources/PipelineFactory.hpp"
#include "render/gi/GlobalIllumination.hpp"
#include "render/gi/SsaoBackend.hpp"
#include "render/world/RenderWorld.hpp"
#include "scene/Level.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

    using lumin::render::FrameGraph;
    using lumin::render::FrameGraphBuilder;
    using lumin::render::FrameGraphContext;
    using lumin::render::FrameGraphPassType;
    using lumin::render::FrameGraphResourceHandle;
    using lumin::render::FrameGraphTextureDesc;
    using lumin::render::gi::BackendInfo;
    using lumin::render::gi::CreateInfo;
    using lumin::render::gi::FrameInfo;
    using lumin::render::gi::GlobalIlluminationBackend;

    static_assert(!std::is_copy_constructible_v<GlobalIlluminationBackend>);
    static_assert(!std::is_move_constructible_v<GlobalIlluminationBackend>);
    static_assert(std::same_as<decltype(lumin::render::gi::FrameResources::position), nvrhi::TextureHandle>);
    static_assert(std::same_as<decltype(lumin::render::gi::FrameResources::normalRoughness), nvrhi::TextureHandle>);
    static_assert(std::same_as<decltype(lumin::render::gi::FrameResources::uniformBuffer), nvrhi::BufferHandle>);
    static_assert(std::same_as<decltype(lumin::render::gi::FrameResources::output), nvrhi::TextureHandle>);
    static_assert(std::same_as<decltype(lumin::render::gi::CreateInfo::outputFormat), nvrhi::Format>);
    static_assert(std::same_as<decltype(lumin::render::gi::CreateInfo::sampler), nvrhi::SamplerHandle>);
    static_assert(
        std::same_as<decltype(lumin::render::gi::FrameInfo::world), const lumin::render::world::RenderWorldSnapshot&>);

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    class FakeGlobalIlluminationBackend final : public GlobalIlluminationBackend {
    public:
        [[nodiscard]] BackendInfo info() const noexcept override {
            return BackendInfo{"Fake GI", true, false};
        }

        void create(const CreateInfo&) override {
        }

        void destroy() noexcept override {
        }

        void invalidateHistory() noexcept override {
            historyInvalidated = true;
        }

        void addPasses(FrameGraph& frameGraph, const FrameInfo& frameInfo) override {
            frameGraph.addPass(
                "Fake GI", FrameGraphPassType::Graphics,
                [position = frameInfo.position, normal = frameInfo.normalRoughness,
                 output = frameInfo.output](FrameGraphBuilder& builder) {
                    builder.readTexture(position, nvrhi::ResourceStates::ShaderResource);
                    builder.readTexture(normal, nvrhi::ResourceStates::ShaderResource);
                    builder.writeTexture(output, nvrhi::ResourceStates::RenderTarget);
                },
                [this](const FrameGraphContext&) {
                    executionOrder->push_back("GI");
                });
        }

        std::vector<std::string>* executionOrder = nullptr;
        bool historyInvalidated = false;
    };

    class FakeBarrierRecorder final : public lumin::render::FrameGraphBarrierRecorder {
    public:
        void beginTrackingTextureState(nvrhi::ITexture*, nvrhi::TextureSubresourceSet, nvrhi::ResourceStates) override {
        }

        void beginTrackingBufferState(nvrhi::IBuffer*, nvrhi::ResourceStates) override {
        }

        void setTextureState(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet,
                             nvrhi::ResourceStates state) override {
            if (state == nvrhi::ResourceStates::ShaderResource) {
                events.push_back(texture == position ? "position-read" : texture == normal ? "normal-read" : "read");
            } else if (state == nvrhi::ResourceStates::CopyDest && texture == output) {
                events.emplace_back("output-clear");
            } else if (state == nvrhi::ResourceStates::RenderTarget && texture == output) {
                events.emplace_back("output-write");
            }
        }

        void setBufferState(nvrhi::IBuffer*, nvrhi::ResourceStates) override {
        }

        void setAccelerationStructureState(nvrhi::rt::IAccelStruct*, nvrhi::ResourceStates) override {
        }

        void commitBarriers() override {
            events.emplace_back("commit");
        }

        nvrhi::ITexture* position = reinterpret_cast<nvrhi::ITexture*>(0x100);
        nvrhi::ITexture* normal = reinterpret_cast<nvrhi::ITexture*>(0x200);
        nvrhi::ITexture* output = reinterpret_cast<nvrhi::ITexture*>(0x300);
        std::vector<std::string> events;
    };

    void testDisabledNeutralOutputPolicy() {
        require(lumin::render::gi::neutralOutput[lumin::render::gi::indirectRadianceFirstChannel] == 0.0f &&
                    lumin::render::gi::neutralOutput[1] == 0.0f && lumin::render::gi::neutralOutput[2] == 0.0f,
                "GI disabled must contribute zero indirect radiance.");
        require(lumin::render::gi::neutralOutput[lumin::render::gi::ambientVisibilityChannel] == 1.0f,
                "GI disabled must preserve full ambient visibility in the neutral output.");
    }

    void testSsaoBackendInfo() {
        const std::unique_ptr<GlobalIlluminationBackend> backend = lumin::render::gi::makeSsaoBackend("unused");
        const BackendInfo info = backend->info();
        require(info.name == "SSAO", "The default GI backend must identify itself as SSAO.");
        require(!info.temporal, "The current SSAO backend must not claim temporal history.");
        require(!info.hardwareRayTracing, "The current SSAO backend must not claim hardware ray tracing.");
    }

    class FakeSsaoCommandList final : public lumin::render::gi::SsaoRecordProbe {
    public:
        void clearTextureFloat(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet,
                               const nvrhi::Color& color) override {
            clearedTexture = texture;
            clearColor = color;
            events.emplace_back("clear");
        }

        void setGraphicsState(const nvrhi::GraphicsState& value) override {
            state = value;
            events.emplace_back("state");
        }

        void draw(const nvrhi::DrawArguments& value) override {
            drawArguments = value;
            events.emplace_back("draw");
        }

        nvrhi::ITexture* clearedTexture = nullptr;
        nvrhi::Color clearColor{};
        nvrhi::GraphicsState state;
        nvrhi::DrawArguments drawArguments;
        std::vector<std::string> events;
    };

    void testSsaoFullscreenRecorder() {
        auto* output = reinterpret_cast<nvrhi::ITexture*>(0x10);
        auto* pipeline = reinterpret_cast<nvrhi::IGraphicsPipeline*>(0x20);
        auto* framebuffer = reinterpret_cast<nvrhi::IFramebuffer*>(0x30);
        auto* bindingSet = reinterpret_cast<nvrhi::IBindingSet*>(0x40);

        nvrhi::GraphicsState state;
        state.setPipeline(pipeline)
            .setFramebuffer(framebuffer)
            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(64.0f, 32.0f)))
            .addBindingSet(bindingSet);

        FakeSsaoCommandList commandList;
        lumin::render::gi::detail::recordSsaoClear(commandList, output);
        lumin::render::gi::detail::recordSsaoFullscreen(commandList, state);

        require(commandList.events == std::vector<std::string>{"clear", "state", "draw"},
                "SSAO must clear before binding graphics state and drawing.");
        require(commandList.clearedTexture == output && commandList.clearColor.r == 0.0f &&
                    commandList.clearColor.g == 0.0f && commandList.clearColor.b == 0.0f &&
                    commandList.clearColor.a == 1.0f,
                "SSAO clear must load the neutral packed GI output.");
        require(commandList.state.pipeline == pipeline && commandList.state.framebuffer == framebuffer &&
                    commandList.state.bindings.size() == 1 && commandList.state.bindings[0] == bindingSet,
                "SSAO graphics state must retain its pipeline, output framebuffer, and binding resources.");
        require(commandList.state.viewport.viewports.size() == 1 &&
                    commandList.state.viewport.viewports[0].width() == 64.0f &&
                    commandList.state.viewport.viewports[0].height() == 32.0f &&
                    commandList.drawArguments.vertexCount == 3 && commandList.drawArguments.instanceCount == 1,
                "SSAO must record a full-extent fullscreen triangle.");
    }

    enum class CreationFailure {
        None,
        Uniform,
        Binding,
        Pipeline
    };

    struct LiveHandles {
        int buffers = 0;
        int layouts = 0;
        int bindingSets = 0;
        int framebuffers = 0;
        int pipelines = 0;

        [[nodiscard]] int ownedTotal() const {
            return buffers + layouts + bindingSets + framebuffers + pipelines;
        }
    };

    class FakeTexture final : public nvrhi::RefCounter<nvrhi::ITexture> {
    public:
        explicit FakeTexture(nvrhi::TextureDesc desc) : desc_(std::move(desc)) {
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
    };

    class FakeBuffer final : public nvrhi::RefCounter<nvrhi::IBuffer> {
    public:
        explicit FakeBuffer(LiveHandles& live) : live_(live) {
            ++live_.buffers;
        }

        ~FakeBuffer() override {
            --live_.buffers;
        }

        [[nodiscard]] const nvrhi::BufferDesc& getDesc() const override {
            return desc_;
        }

        [[nodiscard]] nvrhi::GpuVirtualAddress getGpuVirtualAddress() const override {
            return 0;
        }

    private:
        nvrhi::BufferDesc desc_;
        LiveHandles& live_;
    };

    class FakeSampler final : public nvrhi::RefCounter<nvrhi::ISampler> {
    public:
        [[nodiscard]] const nvrhi::SamplerDesc& getDesc() const override {
            return desc_;
        }

    private:
        nvrhi::SamplerDesc desc_;
    };

    class FakeBindingLayout final : public nvrhi::RefCounter<nvrhi::IBindingLayout> {
    public:
        FakeBindingLayout(nvrhi::BindingLayoutDesc desc, LiveHandles& live) : desc_(std::move(desc)), live_(live) {
            ++live_.layouts;
        }

        ~FakeBindingLayout() override {
            --live_.layouts;
        }

        [[nodiscard]] const nvrhi::BindingLayoutDesc* getDesc() const override {
            return &desc_;
        }

        [[nodiscard]] const nvrhi::BindlessLayoutDesc* getBindlessDesc() const override {
            return nullptr;
        }

    private:
        nvrhi::BindingLayoutDesc desc_;
        LiveHandles& live_;
    };

    class FakeBindingSet final : public nvrhi::RefCounter<nvrhi::IBindingSet> {
    public:
        FakeBindingSet(nvrhi::BindingSetDesc desc, nvrhi::IBindingLayout* layout, LiveHandles& live)
            : desc_(std::move(desc)), layout_(layout), live_(live) {
            ++live_.bindingSets;
        }

        ~FakeBindingSet() override {
            --live_.bindingSets;
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
        LiveHandles& live_;
    };

    class FakeFramebuffer final : public nvrhi::RefCounter<nvrhi::IFramebuffer> {
    public:
        FakeFramebuffer(nvrhi::FramebufferDesc desc, LiveHandles& live) : desc_(std::move(desc)), live_(live) {
            ++live_.framebuffers;
            info_.setWidth(16).setHeight(16).setArraySize(1);
            if (!desc_.colorAttachments.empty()) {
                info_.addColorFormat(desc_.colorAttachments[0].texture->getDesc().format);
            }
        }

        ~FakeFramebuffer() override {
            --live_.framebuffers;
        }

        [[nodiscard]] const nvrhi::FramebufferDesc& getDesc() const override {
            return desc_;
        }

        [[nodiscard]] const nvrhi::FramebufferInfoEx& getFramebufferInfo() const override {
            return info_;
        }

    private:
        nvrhi::FramebufferDesc desc_;
        nvrhi::FramebufferInfoEx info_;
        LiveHandles& live_;
    };

    class FakePipeline final : public nvrhi::RefCounter<nvrhi::IGraphicsPipeline> {
    public:
        FakePipeline(nvrhi::IBindingLayout* layout, nvrhi::Format outputFormat, LiveHandles& live) : live_(live) {
            ++live_.pipelines;
            desc_.addBindingLayout(layout);
            info_.addColorFormat(outputFormat);
        }

        ~FakePipeline() override {
            --live_.pipelines;
        }

        [[nodiscard]] const nvrhi::GraphicsPipelineDesc& getDesc() const override {
            return desc_;
        }

        [[nodiscard]] const nvrhi::FramebufferInfo& getFramebufferInfo() const override {
            return info_;
        }

    private:
        nvrhi::GraphicsPipelineDesc desc_;
        nvrhi::FramebufferInfo info_;
        LiveHandles& live_;
    };

    class FakeCreationDriver final : public lumin::render::gi::SsaoCreationDriver {
    public:
        [[nodiscard]] nvrhi::BufferHandle createUniform() override {
            if (failure == CreationFailure::Uniform) {
                return nullptr;
            }
            return nvrhi::BufferHandle::Create(new FakeBuffer(live));
        }

        [[nodiscard]] nvrhi::BindingLayoutHandle createBindingLayout(const nvrhi::BindingLayoutDesc& desc) override {
            layoutDesc = desc;
            if (failure == CreationFailure::Binding) {
                return nullptr;
            }
            return nvrhi::BindingLayoutHandle::Create(new FakeBindingLayout(desc, live));
        }

        [[nodiscard]] nvrhi::BindingSetHandle createBindingSet(const nvrhi::BindingSetDesc& desc,
                                                               nvrhi::IBindingLayout* layout) override {
            bindingDesc = desc;
            return nvrhi::BindingSetHandle::Create(new FakeBindingSet(desc, layout, live));
        }

        [[nodiscard]] nvrhi::FramebufferHandle createFramebuffer(const nvrhi::FramebufferDesc& desc) override {
            framebufferDesc = desc;
            return nvrhi::FramebufferHandle::Create(new FakeFramebuffer(desc, live));
        }

        [[nodiscard]] nvrhi::GraphicsPipelineHandle
        createPipeline(const lumin::render::GraphicsPipelineDesc& desc) override {
            pipelineFormat = desc.colorFormats.front();
            pipelineCullMode = desc.cullMode;
            if (failure == CreationFailure::Pipeline) {
                return nullptr;
            }
            return nvrhi::GraphicsPipelineHandle::Create(
                new FakePipeline(desc.bindingLayouts.front(), pipelineFormat, live));
        }

        CreationFailure failure = CreationFailure::None;
        LiveHandles live;
        nvrhi::BindingLayoutDesc layoutDesc;
        nvrhi::BindingSetDesc bindingDesc;
        nvrhi::FramebufferDesc framebufferDesc;
        nvrhi::Format pipelineFormat = nvrhi::Format::UNKNOWN;
        nvrhi::RasterCullMode pipelineCullMode = nvrhi::RasterCullMode::Back;
    };

    void testSsaoCreationRollbackAndRetry() {
        for (CreationFailure failure :
             std::array{CreationFailure::Uniform, CreationFailure::Binding, CreationFailure::Pipeline}) {
            FakeCreationDriver driver;
            driver.failure = failure;
            const nvrhi::TextureDesc textureDesc = nvrhi::TextureDesc()
                                                       .setWidth(16)
                                                       .setHeight(16)
                                                       .setFormat(nvrhi::Format::RGBA16_FLOAT)
                                                       .setIsRenderTarget(true);
            const nvrhi::TextureHandle position = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
            const nvrhi::TextureHandle normal = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
            const nvrhi::TextureHandle output = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
            const nvrhi::SamplerHandle sampler = nvrhi::SamplerHandle::Create(new FakeSampler);
            const std::array<lumin::render::gi::FrameResources, 1> frames = {lumin::render::gi::FrameResources{
                .position = position, .normalRoughness = normal, .uniformBuffer = nullptr, .output = output}};
            std::unique_ptr<GlobalIlluminationBackend> backend = lumin::render::gi::makeSsaoBackend("unused");
            const CreateInfo createInfo{.extent = {16, 16},
                                        .outputFormat = nvrhi::Format::RGBA16_FLOAT,
                                        .sampler = sampler,
                                        .frames = frames,
                                        .creationDriver = &driver};

            bool failed = false;
            try {
                backend->create(createInfo);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            require(failed, "Injected SSAO creation failure must propagate.");
            require(driver.live.ownedTotal() == 0, "Partial SSAO creation must release every NvRHI member handle.");
            backend->destroy();
            backend->destroy();
            require(driver.live.ownedTotal() == 0, "SSAO destroy must remain idempotent after failed creation.");

            driver.failure = CreationFailure::None;
            backend->create(createInfo);
            require(driver.live.buffers == 1 && driver.live.layouts == 1 && driver.live.bindingSets == 1 &&
                        driver.live.framebuffers == 1 && driver.live.pipelines == 1,
                    "SSAO retry must populate its actual NvRHI member handles.");
            backend->destroy();
            backend->destroy();
            require(driver.live.ownedTotal() == 0, "Successful SSAO resources must be released exactly once.");
        }
        std::cout << "PASS: uniform/binding/pipeline rollback, idempotent destroy, and retry\n";
    }

    void testSsaoProductionPass() {
        FakeCreationDriver driver;
        FakeSsaoCommandList recorder;
        const nvrhi::TextureDesc textureDesc = nvrhi::TextureDesc()
                                                   .setWidth(16)
                                                   .setHeight(16)
                                                   .setFormat(nvrhi::Format::RGBA16_FLOAT)
                                                   .setIsRenderTarget(true);
        const nvrhi::TextureHandle position = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
        const nvrhi::TextureHandle normal = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
        const nvrhi::TextureHandle output = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
        const nvrhi::SamplerHandle sampler = nvrhi::SamplerHandle::Create(new FakeSampler);
        const std::array<lumin::render::gi::FrameResources, 1> frames = {lumin::render::gi::FrameResources{
            .position = position, .normalRoughness = normal, .uniformBuffer = nullptr, .output = output}};
        std::unique_ptr<GlobalIlluminationBackend> backend = lumin::render::gi::makeSsaoBackend("unused");
        backend->create(CreateInfo{.extent = {16, 16},
                                   .outputFormat = nvrhi::Format::RGBA16_FLOAT,
                                   .sampler = sampler,
                                   .frames = frames,
                                   .creationDriver = &driver,
                                   .recordProbe = &recorder});

        require(driver.layoutDesc.bindings.size() == 4 &&
                    driver.layoutDesc.bindings[0] == nvrhi::BindingLayoutItem::Texture_SRV(0) &&
                    driver.layoutDesc.bindings[1] == nvrhi::BindingLayoutItem::Texture_SRV(1) &&
                    driver.layoutDesc.bindings[2] == nvrhi::BindingLayoutItem::Sampler(2) &&
                    driver.layoutDesc.bindings[3] == nvrhi::BindingLayoutItem::ConstantBuffer(3),
                "Production SSAO creation must preserve the shader binding ABI.");
        require(driver.bindingDesc.bindings.size() == 4 && driver.bindingDesc.bindings[0].resourceHandle == position &&
                    driver.bindingDesc.bindings[1].resourceHandle == normal &&
                    driver.bindingDesc.bindings[2].resourceHandle == sampler &&
                    driver.bindingDesc.bindings[3].type == nvrhi::ResourceType::ConstantBuffer,
                "Production SSAO binding set must bind the requested textures, sampler, and created uniform.");
        require(driver.framebufferDesc.colorAttachments.size() == 1 &&
                    driver.framebufferDesc.colorAttachments[0].texture == output &&
                    driver.pipelineFormat == nvrhi::Format::RGBA16_FLOAT,
                "Production SSAO framebuffer and pipeline must target the output attachment format.");
        require(driver.pipelineCullMode == nvrhi::RasterCullMode::None,
                "Production SSAO fullscreen pipeline must disable culling after the fullscreen winding change.");

        FrameGraph graph;
        FakeBarrierRecorder barriers;
        barriers.position = position;
        barriers.normal = normal;
        barriers.output = output;
        FrameGraphTextureDesc desc;
        desc.texture = position;
        const FrameGraphResourceHandle positionHandle = graph.importTexture("position", desc);
        desc.texture = normal;
        const FrameGraphResourceHandle normalHandle = graph.importTexture("normal", desc);
        desc.texture = output;
        const FrameGraphResourceHandle outputHandle = graph.importTexture("output", desc);
        lumin::scene::Level level;
        const auto renderWorld = lumin::render::world::RenderWorldExtractor::extract(level);
        backend->addPasses(graph, FrameInfo{.world = *renderWorld,
                                            .extent = {16, 16},
                                            .position = positionHandle,
                                            .normalRoughness = normalHandle,
                                            .output = outputHandle});
        graph.execute(FrameGraphContext{.barriers = &barriers});

        require(recorder.events == std::vector<std::string>{"clear", "state", "draw"} &&
                    recorder.clearedTexture == output && recorder.clearColor.r == 0.0f &&
                    recorder.clearColor.g == 0.0f && recorder.clearColor.b == 0.0f && recorder.clearColor.a == 1.0f,
                "Production SSAO pass must clear/load neutral output before storing its draw result.");
        require(recorder.state.framebuffer != nullptr && recorder.state.bindings.size() == 1 &&
                    recorder.drawArguments.vertexCount == 3 && recorder.drawArguments.instanceCount == 1,
                "Production SSAO callback must bind its framebuffer/resources and draw one fullscreen triangle.");
        const auto positionRead = std::find(barriers.events.begin(), barriers.events.end(), "position-read");
        const auto normalRead = std::find(barriers.events.begin(), barriers.events.end(), "normal-read");
        const auto outputWrite = std::find(barriers.events.begin(), barriers.events.end(), "output-write");
        require(positionRead < outputWrite && normalRead < outputWrite,
                "Production SSAO FrameGraph declaration must submit both reads before the output write.");
        const auto outputClear = std::find(barriers.events.begin(), barriers.events.end(), "output-clear");
        require(outputClear < outputWrite,
                "SSAO neutral clear must run in a CopyDest transfer pass before the RenderTarget draw pass.");
        graph.reset();

        recorder.events.clear();
        barriers.events.clear();
        desc.texture = position;
        const FrameGraphResourceHandle disabledPosition = graph.importTexture("disabled.position", desc);
        desc.texture = normal;
        const FrameGraphResourceHandle disabledNormal = graph.importTexture("disabled.normal", desc);
        desc.texture = output;
        const FrameGraphResourceHandle disabledOutput = graph.importTexture("disabled.output", desc);
        backend->addPasses(graph, FrameInfo{.world = *renderWorld,
                                            .enabled = false,
                                            .extent = {16, 16},
                                            .position = disabledPosition,
                                            .normalRoughness = disabledNormal,
                                            .output = disabledOutput});
        graph.execute(FrameGraphContext{.barriers = &barriers});
        require(recorder.events == std::vector<std::string>{"clear"},
                "Disabled SSAO must only clear its output to the neutral GI value.");
        require(std::find(barriers.events.begin(), barriers.events.end(), "position-read") == barriers.events.end() &&
                    std::find(barriers.events.begin(), barriers.events.end(), "normal-read") == barriers.events.end() &&
                    std::find(barriers.events.begin(), barriers.events.end(), "output-write") == barriers.events.end(),
                "Disabled SSAO must not read the G-buffer or register a fullscreen render-target write.");
        graph.reset();
        backend->destroy();
        require(driver.live.ownedTotal() == 0, "Production SSAO pass resources must release after recording.");
        std::cout << "PASS: production SSAO FrameGraph, binding, framebuffer clear/store, and draw\n";
    }

    void testSsaoInvalidBoundaries() {
        const nvrhi::TextureDesc textureDesc = nvrhi::TextureDesc()
                                                   .setWidth(16)
                                                   .setHeight(16)
                                                   .setFormat(nvrhi::Format::RGBA16_FLOAT)
                                                   .setIsRenderTarget(true);
        const nvrhi::TextureHandle position = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
        const nvrhi::TextureHandle normal = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
        const nvrhi::TextureHandle output = nvrhi::TextureHandle::Create(new FakeTexture(textureDesc));
        const nvrhi::SamplerHandle sampler = nvrhi::SamplerHandle::Create(new FakeSampler);
        const std::array<lumin::render::gi::FrameResources, 1> frames = {
            lumin::render::gi::FrameResources{.position = position, .normalRoughness = normal, .output = output}};
        FakeCreationDriver driver;

        const auto expectCreateInvalid = [](const CreateInfo& info) {
            std::unique_ptr<GlobalIlluminationBackend> backend = lumin::render::gi::makeSsaoBackend("unused");
            bool rejected = false;
            try {
                backend->create(info);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "Malformed SSAO creation input must be rejected.");
        };

        expectCreateInvalid(CreateInfo{
            .extent = {16, 16}, .outputFormat = nvrhi::Format::RGBA16_FLOAT, .sampler = sampler, .frames = frames});
        expectCreateInvalid(CreateInfo{.extent = {0, 16},
                                       .outputFormat = nvrhi::Format::RGBA16_FLOAT,
                                       .sampler = sampler,
                                       .frames = frames,
                                       .creationDriver = &driver});
        expectCreateInvalid(CreateInfo{.extent = {16, 16},
                                       .outputFormat = nvrhi::Format::UNKNOWN,
                                       .sampler = sampler,
                                       .frames = frames,
                                       .creationDriver = &driver});
        expectCreateInvalid(CreateInfo{.extent = {16, 16},
                                       .outputFormat = nvrhi::Format::RGBA16_FLOAT,
                                       .frames = frames,
                                       .creationDriver = &driver});
        expectCreateInvalid(CreateInfo{.extent = {16, 16},
                                       .outputFormat = nvrhi::Format::RGBA16_FLOAT,
                                       .sampler = sampler,
                                       .frames = {},
                                       .creationDriver = &driver});
        const std::array<lumin::render::gi::FrameResources, 1> incomplete = {
            lumin::render::gi::FrameResources{.normalRoughness = normal, .output = output}};
        expectCreateInvalid(CreateInfo{.extent = {16, 16},
                                       .outputFormat = nvrhi::Format::RGBA16_FLOAT,
                                       .sampler = sampler,
                                       .frames = incomplete,
                                       .creationDriver = &driver});

        std::unique_ptr<GlobalIlluminationBackend> backend = lumin::render::gi::makeSsaoBackend("unused");
        backend->create(CreateInfo{.extent = {16, 16},
                                   .outputFormat = nvrhi::Format::RGBA16_FLOAT,
                                   .sampler = sampler,
                                   .frames = frames,
                                   .creationDriver = &driver});
        lumin::scene::Level level;
        const auto renderWorld = lumin::render::world::RenderWorldExtractor::extract(level);
        FrameGraph graph;
        bool badSlotRejected = false;
        try {
            backend->addPasses(graph, FrameInfo{.world = *renderWorld, .frameIndex = 1, .extent = {16, 16}});
        } catch (const std::logic_error&) {
            badSlotRejected = true;
        }
        require(badSlotRejected, "SSAO must reject a frame slot outside its created resources.");
        bool emptyPassRejected = false;
        try {
            backend->addPasses(graph, FrameInfo{.world = *renderWorld, .extent = {0, 16}});
        } catch (const std::invalid_argument&) {
            emptyPassRejected = true;
        }
        require(emptyPassRejected, "SSAO must reject an empty pass extent.");

        FrameGraphTextureDesc desc;
        desc.texture = position;
        const FrameGraphResourceHandle positionHandle = graph.importTexture("position", desc);
        desc.texture = normal;
        const FrameGraphResourceHandle normalHandle = graph.importTexture("normal", desc);
        desc.texture = output;
        const FrameGraphResourceHandle outputHandle = graph.importTexture("output", desc);
        backend->addPasses(graph, FrameInfo{.world = *renderWorld,
                                            .extent = {16, 16},
                                            .position = positionHandle,
                                            .normalRoughness = normalHandle,
                                            .output = outputHandle});
        bool nullCommandRejected = false;
        try {
            graph.execute(FrameGraphContext{});
        } catch (const std::logic_error&) {
            nullCommandRejected = true;
        }
        require(nullCommandRejected, "SSAO execution must reject a null command list when no record probe exists.");
        graph.reset();
        backend->destroy();
        require(driver.live.ownedTotal() == 0, "Invalid SSAO boundary probes must leave no live handles.");
        std::cout << "PASS: SSAO invalid create/pass boundaries\n";
    }

    void testFrameGraphOrdering() {
        FrameGraph frameGraph;
        FakeBarrierRecorder barriers;
        FrameGraphTextureDesc texture;

        texture.texture = barriers.position;
        const FrameGraphResourceHandle position = frameGraph.importTexture("gbuffer.position", texture);
        texture.texture = barriers.normal;
        const FrameGraphResourceHandle normal = frameGraph.importTexture("gbuffer.normal", texture);
        texture.texture = reinterpret_cast<nvrhi::ITexture*>(0x400);
        const FrameGraphResourceHandle albedo = frameGraph.importTexture("gbuffer.albedo", texture);
        texture.texture = reinterpret_cast<nvrhi::ITexture*>(0x500);
        const FrameGraphResourceHandle motion = frameGraph.importTexture("gbuffer.motion", texture);
        texture.texture = reinterpret_cast<nvrhi::ITexture*>(0x600);
        const FrameGraphResourceHandle depth = frameGraph.importTexture("gbuffer.depth", texture);
        texture.texture = barriers.output;
        const FrameGraphResourceHandle output = frameGraph.importTexture("gi.output", texture);

        std::vector<std::string> order;
        frameGraph.addPass(
            "G-buffer", FrameGraphPassType::Graphics,
            [position, normal](FrameGraphBuilder& builder) {
                builder.writeTexture(position, nvrhi::ResourceStates::RenderTarget);
                builder.writeTexture(normal, nvrhi::ResourceStates::RenderTarget);
            },
            [&order](const FrameGraphContext&) {
                order.push_back("G-buffer");
            });

        lumin::scene::Level level;
        const auto renderWorld = lumin::render::world::RenderWorldExtractor::extract(level);
        FakeGlobalIlluminationBackend backend;
        backend.executionOrder = &order;
        const FrameInfo frameInfo{*renderWorld, 0,      0,      true,   false, lumin::render::gi::RenderExtent{16, 16},
                                  position,     normal, albedo, motion, depth, output};
        backend.addPasses(frameGraph, frameInfo);

        frameGraph.addPass(
            "Deferred lighting", FrameGraphPassType::Graphics,
            [output](FrameGraphBuilder& builder) {
                builder.readTexture(output, nvrhi::ResourceStates::ShaderResource);
            },
            [&order](const FrameGraphContext&) {
                order.push_back("Deferred");
            });

        frameGraph.execute(FrameGraphContext{.barriers = &barriers});
        require(order == std::vector<std::string>{"G-buffer", "GI", "Deferred"},
                "FrameGraph must order G-buffer before GI and GI before deferred lighting.");
        const auto positionRead = std::find(barriers.events.begin(), barriers.events.end(), "position-read");
        const auto normalRead = std::find(barriers.events.begin(), barriers.events.end(), "normal-read");
        const auto outputWrite = std::find(barriers.events.begin(), barriers.events.end(), "output-write");
        const auto commit = outputWrite == barriers.events.end()
                                ? barriers.events.end()
                                : std::find(std::next(outputWrite), barriers.events.end(), "commit");
        require(positionRead < outputWrite && normalRead < outputWrite && outputWrite < commit,
                "FrameGraph must submit both SSAO reads before the output write and barrier commit.");
    }

    void testHistoryInvalidationPolicy() {
        using lumin::render::gi::HistoryInvalidationState;
        using lumin::render::gi::shouldInvalidateHistory;

        require(!shouldInvalidateHistory(HistoryInvalidationState{}), "A stable frame must not invalidate GI history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.cameraCut = true}),
                "A camera cut must invalidate GI history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.topologyChanged = true}),
                "A topology change must invalidate GI history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.backendReenabled = true}),
                "Re-enabling GI must invalidate backend history.");
        require(shouldInvalidateHistory(HistoryInvalidationState{.swapchainRecreated = true}),
                "Swapchain recreation must invalidate GI history.");
    }

    std::string readSource(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Unable to open GI shader source: " + path.string());
        }
        return std::string(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    }

    void testShaderOutputContract() {
#if defined(LUMIN_TEST_SOURCE_DIR)
        const std::filesystem::path sourceDirectory = LUMIN_TEST_SOURCE_DIR;
#else
        const std::filesystem::path sourceDirectory = ".";
#endif
        const std::string ssao = readSource(sourceDirectory / "shaders" / "ssao.slang");
        require(ssao.find("frame.renderOptions.y < 0.5") != std::string::npos,
                "SSAO must retain the enabled-path guard.");
        require(ssao.find("return float4(0.0, 0.0, 0.0, 1.0);") != std::string::npos,
                "Disabled SSAO must emit the neutral packed GI output.");
        require(ssao.find("return float4(0.0, 0.0, 0.0, ao);") != std::string::npos,
                "Enabled SSAO must pack ambient visibility in GI alpha.");

        const std::string deferred = readSource(sourceDirectory / "shaders" / "deferred.slang");
        require(deferred.find("Texture2D<float4> globalIlluminationTexture") != std::string::npos,
                "Deferred lighting must consume the packed RGBA GI output.");
        require(deferred.find("legacyAmbient * globalIllumination.a + globalIllumination.rgb") != std::string::npos,
                "Deferred lighting must combine ambient visibility and indirect radiance.");
    }

} // namespace

int main() {
    try {
        testDisabledNeutralOutputPolicy();
        testSsaoBackendInfo();
        testSsaoFullscreenRecorder();
        testSsaoCreationRollbackAndRetry();
        testSsaoProductionPass();
        testSsaoInvalidBoundaries();
        testFrameGraphOrdering();
        testHistoryInvalidationPolicy();
        testShaderOutputContract();
        std::cout << "GlobalIllumination PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GlobalIllumination FAIL: " << error.what() << '\n';
        return 1;
    }
}

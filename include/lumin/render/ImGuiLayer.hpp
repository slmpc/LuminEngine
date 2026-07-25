#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "lumin/render/ImGuiContent.hpp"

struct ImDrawCmd;
struct ImDrawData;
struct ImDrawList;

namespace lumin::platform {
    class Window;
}

namespace lumin::render {

    struct ImGuiLayerConfig {
        nvrhi::IDevice* device = nullptr;
        nvrhi::Format colorFormat = nvrhi::Format::UNKNOWN;
        std::filesystem::path shaderDirectory;
        std::uint32_t frameSlotCount = 2;
        std::uint32_t sampleCount = 1;
        bool enableKeyboard = true;
        bool enableGamepad = false;
        bool enableDocking = true;
        float globalScale = 1.0f;
    };

    struct ImGuiDrawEvent {
        enum class Type {
            Draw,
            ResetRenderState,
            UserCallback,
        };

        Type type = Type::Draw;
        const ImDrawList* list = nullptr;
        const ImDrawCmd* command = nullptr;
        std::int32_t scissorLeft = 0;
        std::int32_t scissorTop = 0;
        std::int32_t scissorRight = 0;
        std::int32_t scissorBottom = 0;
        std::uint32_t elementCount = 0;
        std::uint32_t indexOffset = 0;
        std::uint32_t vertexOffset = 0;
    };

    struct ImGuiProjection {
        float scaleX = 0.0f;
        float scaleY = 0.0f;
        float translateX = 0.0f;
        float translateY = 0.0f;
    };

    class ImGuiLayer {
    public:
        ImGuiLayer() = default;
        ~ImGuiLayer();

        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;

        void initialize(platform::Window& window, const ImGuiLayerConfig& config);
        void shutdown();
        void newFrame();
        void render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, std::uint32_t frameSlot);

        [[nodiscard]] bool initialized() const noexcept;
        [[nodiscard]] ImGuiCaptureState captureState() const noexcept;
        [[nodiscard]] nvrhi::ITexture* fontTexture() const noexcept;
        [[nodiscard]] nvrhi::ResourceStates fontTextureInitialState() const noexcept;
        void markFontTextureInitialized() noexcept;

        [[nodiscard]] static std::size_t growBufferCapacity(std::size_t currentCapacity, std::size_t requiredCapacity,
                                                            std::size_t minimumCapacity) noexcept;
        [[nodiscard]] static ImGuiProjection makeNvrhiProjection(float displayPosX, float displayPosY,
                                                                 float displayWidth, float displayHeight) noexcept;
        [[nodiscard]] static std::vector<ImGuiDrawEvent> buildDrawEvents(const ImDrawData& drawData);

    private:
        struct FrameBuffers {
            nvrhi::BufferHandle vertexBuffer;
            nvrhi::BufferHandle indexBuffer;
            std::size_t vertexCapacity = 0;
            std::size_t indexCapacity = 0;
        };

        void createRendererResources(const ImGuiLayerConfig& config);
        void createFontResources();
        void ensureBuffers(FrameBuffers& buffers, std::size_t vertexCount, std::size_t indexCount);
        void setRenderState(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer,
                            const ImDrawData& drawData, const FrameBuffers& buffers, const nvrhi::Rect& scissor);

        platform::Window* window_ = nullptr;
        nvrhi::IDevice* device_ = nullptr;
        std::filesystem::path shaderDirectory_;
        nvrhi::TextureHandle fontTexture_;
        bool fontTextureInitialized_ = false;
        nvrhi::SamplerHandle fontSampler_;
        nvrhi::BindingLayoutHandle bindingLayout_;
        nvrhi::BindingSetHandle bindingSet_;
        nvrhi::ShaderHandle vertexShader_;
        nvrhi::ShaderHandle fragmentShader_;
        nvrhi::InputLayoutHandle inputLayout_;
        nvrhi::GraphicsPipelineHandle pipeline_;
        std::vector<FrameBuffers> frameBuffers_;
        bool contextCreated_ = false;
        bool sdlInitialized_ = false;
        bool initialized_ = false;
    };

} // namespace lumin::render

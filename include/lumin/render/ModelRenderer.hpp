#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "lumin/assets/ObjLoader.hpp"

namespace lumin::scene {
    class Camera;
    class Level;
} // namespace lumin::scene

namespace lumin::render {

    class VulkanContext;

    struct alignas(16) ObjectData {
        glm::mat4 model{1.0f};
        glm::mat4 previousModel{1.0f};
        glm::mat4 normalMatrix{1.0f};
        glm::vec4 baseColorMetallic{1.0f};
        // roughness factor, UV scale, material texture descriptor index, normal-map Y sign
        glm::vec4 materialParameters{1.0f, 1.0f, 0.0f, 1.0f};
    };

    static_assert(sizeof(ObjectData) == 224);
    static_assert(alignof(ObjectData) == 16);

    struct ModelRendererCapabilities {
        std::uint32_t maxMaterialTextureArrayLength = 1024;
        std::uint32_t maxDrawIndirectCount = 65536;
        std::uint32_t maxImageDimension2D = 8192;
    };

    struct ModelBatch {
        using Command = nvrhi::DrawIndexedIndirectArguments;

        std::vector<assets::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<Command> commands;
        std::vector<ObjectData> objects;
    };

    namespace detail {

        template <typename CommandList>
        void recordModelIndexedIndirect(CommandList& commandList, const nvrhi::GraphicsState& state,
                                        std::uint32_t drawCount) {
            if (drawCount == 0) {
                return;
            }
            commandList.setGraphicsState(state);
            commandList.drawIndexedIndirect(0, drawCount);
        }

    } // namespace detail

    class ModelRenderer {
    public:
        ModelRenderer(VulkanContext& context, const scene::Level& level, std::filesystem::path shaderDirectory,
                      std::span<const nvrhi::Format> colorFormats, nvrhi::Format depthFormat,
                      nvrhi::Format shadowDepthFormat, std::uint32_t frameCount,
                      ModelRendererCapabilities capabilities);
        ~ModelRenderer();

        ModelRenderer(const ModelRenderer&) = delete;
        ModelRenderer& operator=(const ModelRenderer&) = delete;

        [[nodiscard]] static ModelBatch buildBatch(const scene::Level& level);
        void sync(const scene::Level& level, std::uint32_t frameIndex, bool resetMotion);
        void recordGBuffer(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, std::uint32_t width,
                           std::uint32_t height, std::uint32_t frameIndex, const glm::mat4& viewProjection,
                           const glm::mat4& previousViewProjection);
        void recordShadow(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, std::uint32_t width,
                          std::uint32_t height, std::uint32_t frameIndex, std::uint32_t cascadeIndex,
                          const glm::mat4& lightViewProjection);
        void record(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, std::uint32_t frameIndex,
                    const scene::Camera& camera, float aspectRatio);
        [[nodiscard]] std::uint32_t drawCount() const noexcept;
        [[nodiscard]] const nvrhi::BufferHandle& vertexBuffer() const noexcept;
        [[nodiscard]] const nvrhi::BufferHandle& indexBuffer() const noexcept;
        [[nodiscard]] const nvrhi::BufferHandle& indirectBuffer() const noexcept;
        [[nodiscard]] const nvrhi::BufferHandle& objectBuffer(std::uint32_t frameIndex) const;
        [[nodiscard]] const nvrhi::BufferHandle& frameUniformBuffer(std::uint32_t frameIndex) const;
        [[nodiscard]] const nvrhi::BufferHandle& shadowUniformBuffer(std::uint32_t frameIndex,
                                                                     std::uint32_t cascadeIndex) const;
        [[nodiscard]] std::span<const nvrhi::TextureHandle> baseColorTextures() const noexcept;
        [[nodiscard]] std::span<const nvrhi::TextureHandle> normalRoughnessTextures() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <nvrhi/nvrhi.h>

#include "assets/ObjLoader.hpp"
#include "render/core/ModelRendererCapabilities.hpp"
#include "render/gpu/GpuMaterial.hpp"

namespace lumin::scene {
    class Camera;
} // namespace lumin::scene

namespace lumin::render::world {
    class RenderWorldSnapshot;
}

namespace lumin::render {

    class VulkanContext;

    struct alignas(16) ObjectData {
        glm::mat4 model{1.0f};
        glm::mat4 previousModel{1.0f};
        glm::mat4 normalMatrix{1.0f};
        glm::vec4 baseColorMetallic{1.0f};
        // roughness factor, UV scale, material texture descriptor index, normal-map Y sign
        glm::vec4 materialParameters{1.0f, 1.0f, 0.0f, 1.0f};
        // x = stable GPU material index; yzw are reserved and must remain zero
        glm::uvec4 metadata{0U};
    };

    static_assert(sizeof(ObjectData) == 240);
    static_assert(alignof(ObjectData) == 16);

    struct ModelBatch {
        using Command = nvrhi::DrawIndexedIndirectArguments;

        std::vector<assets::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<Command> commands;
        std::vector<ObjectData> objects;
        /** 由 `ModelHandle::index` 稀疏寻址的 raster/RT 共享材质表。 */
        std::vector<gpu::GpuMaterialData> materials;
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
        ModelRenderer(VulkanContext& context, const world::RenderWorldSnapshot& world,
                      std::filesystem::path shaderDirectory, std::span<const nvrhi::Format> colorFormats,
                      nvrhi::Format depthFormat, nvrhi::Format shadowDepthFormat, std::uint32_t frameCount,
                      ModelRendererCapabilities capabilities);
        ~ModelRenderer();

        ModelRenderer(const ModelRenderer&) = delete;
        ModelRenderer& operator=(const ModelRenderer&) = delete;

        [[nodiscard]] static ModelBatch buildBatch(const world::RenderWorldSnapshot& world);
        void sync(const world::RenderWorldSnapshot& world, std::uint32_t frameIndex, bool resetMotion);
        /** 在当前帧成功提交后推进模型变换历史；录制失败时不得调用。 */
        void commitSubmittedFrame() noexcept;
        /** 放弃尚未提交的模型变换候选；已提交的运动历史保持不变。 */
        void discardPendingFrame() noexcept;
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
        /** 返回对应 frame slot 的只读 GPU material structured buffer。 */
        [[nodiscard]] const nvrhi::BufferHandle& materialBuffer(std::uint32_t frameIndex) const;
        /** 返回录制本帧前 material buffer 的已知状态，供 `FrameGraph` 导入。 */
        [[nodiscard]] nvrhi::ResourceStates materialBufferInitialState(std::uint32_t frameIndex) const;
        [[nodiscard]] const nvrhi::BufferHandle& frameUniformBuffer(std::uint32_t frameIndex) const;
        [[nodiscard]] const nvrhi::BufferHandle& shadowUniformBuffer(std::uint32_t frameIndex,
                                                                     std::uint32_t cascadeIndex) const;
        [[nodiscard]] std::span<const nvrhi::TextureHandle> baseColorTextures() const noexcept;
        [[nodiscard]] std::span<const nvrhi::TextureHandle> normalRoughnessTextures() const noexcept;
        [[nodiscard]] nvrhi::SamplerHandle materialSampler() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render

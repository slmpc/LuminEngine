#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <vulkan/vulkan.h>

#include "lumin/assets/ObjLoader.hpp"

namespace lumin::scene {
    class Camera;
    class Level;
}

namespace lumin::render {

    class VulkanContext;

    struct alignas(16) ObjectData {
        glm::mat4 model{1.0f};
        glm::vec4 albedoRoughness{1.0f};
    };

    static_assert(sizeof(ObjectData) == 80);
    static_assert(alignof(ObjectData) == 16);

    struct ModelBatch {
        std::vector<assets::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<VkDrawIndexedIndirectCommand> commands;
        std::vector<ObjectData> objects;
    };

    class ModelRenderer {
    public:
        ModelRenderer(VulkanContext& context, const scene::Level& level, std::filesystem::path shaderDirectory,
                      std::span<const VkFormat> colorFormats, VkFormat depthFormat, std::uint32_t frameCount);
        ~ModelRenderer();

        ModelRenderer(const ModelRenderer&) = delete;
        ModelRenderer& operator=(const ModelRenderer&) = delete;

        [[nodiscard]] static ModelBatch buildBatch(const scene::Level& level);
        void record(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, const scene::Camera& camera,
                    float aspectRatio);
        [[nodiscard]] std::uint32_t drawCount() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}

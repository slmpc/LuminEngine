#pragma once

#include <filesystem>
#include <memory>

#include "assets/ObjLoader.hpp"
#include "render/RenderSettings.hpp"

namespace lumin::render {
    class VulkanContext;
} // namespace lumin::render

namespace lumin::scene {
    class Camera;
}

namespace lumin::render {

    /**
     * @brief 为单个 mesh 提供兼容性的场景渲染门面。
     *
     * 该类型拥有内部 `LevelRenderer`，但不拥有 `VulkanContext`、相机或设置；所有 GPU 调用必须发生在创建它的线程。
     */
    class ObjRenderer {
    public:
        /**
         * @brief 从非空 mesh 创建渲染器。
         * @param context 生命周期必须覆盖本对象的 Vulkan/NvRHI 上下文。
         * @param mesh 创建时复制到内部场景的网格。
         * @param shaderDirectory 编译后 shader 所在目录。
         * @throws std::invalid_argument `mesh` 为空时抛出。
         */
        ObjRenderer(VulkanContext& context, const assets::Mesh& mesh, std::filesystem::path shaderDirectory);

        /// 等待内部渲染器完成并释放其资源。
        ~ObjRenderer();

        ObjRenderer(const ObjRenderer&) = delete;
        ObjRenderer& operator=(const ObjRenderer&) = delete;

        /// 使用调用方相机和设置同步渲染一帧。
        void drawFrame(scene::Camera& camera, RenderSettings& settings);

        /// 等待此渲染器提交的全部 GPU 工作完成。
        void waitIdle() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lumin::render

#pragma once

#include <cstdint>

namespace lumin::render {

    /**
     * @brief 模型渲染资源创建使用的设备上限快照。
     *
     * Vulkan 后端负责从物理设备提取这些值，Raster Feature 只消费快照，不反向依赖原生 Vulkan API。
     */
    struct ModelRendererCapabilities {
        /// 单个材质纹理 descriptor array 允许的最大元素数。
        std::uint32_t maxMaterialTextureArrayLength = 1024;
        /// 一次 indexed indirect draw 允许的最大命令数。
        std::uint32_t maxDrawIndirectCount = 65536;
        /// 材质纹理允许的最大二维尺寸。
        std::uint32_t maxImageDimension2D = 8192;
    };

} // namespace lumin::render

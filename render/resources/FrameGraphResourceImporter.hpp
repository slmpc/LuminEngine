#pragma once

#include "render/resources/FrameGraph.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace lumin::render {

    /**
     * 保证一帧内同一物理资源只生成一个 FrameGraph handle。
     *
     * 完全相同的重复导入返回首个 handle；状态、范围或大小不兼容时立即拒绝，避免不同 Feature
     * 对同一资源建立互相矛盾的初始状态。
     */
    class FrameGraphResourceImporter final {
    public:
        /// 绑定本帧唯一 FrameGraph；`graph` 必须覆盖 importer 的生命周期。
        explicit FrameGraphResourceImporter(FrameGraph& graph) noexcept;

        /**
         * @brief 导入物理 buffer，重复导入兼容描述时返回首个 handle。
         * @throws std::invalid_argument 物理对象为空或重复描述的大小、初始/最终状态不兼容时抛出。
         */
        [[nodiscard]] FrameGraphResourceHandle importBuffer(std::string name, const FrameGraphBufferDesc& desc);

        /**
         * @brief 导入物理 texture，重复导入兼容描述时返回首个 handle。
         * @throws std::invalid_argument 物理对象为空或重复描述的子资源、初始/最终状态不兼容时抛出。
         */
        [[nodiscard]] FrameGraphResourceHandle importTexture(std::string name, const FrameGraphTextureDesc& desc);

        /**
         * @brief 导入物理加速结构，重复导入兼容描述时返回首个 handle。
         * @throws std::invalid_argument 物理对象为空或重复描述的初始/最终状态不兼容时抛出。
         */
        [[nodiscard]] FrameGraphResourceHandle
        importAccelerationStructure(std::string name, const FrameGraphAccelerationStructureDesc& desc);

        /// 返回本帧已建立唯一映射的物理资源总数。
        [[nodiscard]] std::size_t importedResourceCount() const noexcept;

    private:
        template <typename Description> struct Entry {
            FrameGraphResourceHandle handle;
            Description description;
        };

        FrameGraph* graph_ = nullptr;
        std::unordered_map<nvrhi::IBuffer*, Entry<FrameGraphBufferDesc>> buffers_;
        std::unordered_map<nvrhi::ITexture*, Entry<FrameGraphTextureDesc>> textures_;
        std::unordered_map<nvrhi::rt::IAccelStruct*, Entry<FrameGraphAccelerationStructureDesc>>
            accelerationStructures_;
    };

} // namespace lumin::render

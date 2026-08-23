#pragma once

#include "render/resources/ShaderCatalog.hpp"

#include <array>
#include <cstddef>
#include <filesystem>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    namespace detail {

        /** 将 Catalog stage 转换为单一 NvRHI shader stage。 */
        [[nodiscard]] nvrhi::ShaderType toNvrhiShaderType(ShaderStage stage) noexcept;

        /** 由类型化 Catalog 入口生成不持有外部字符串指针的 NvRHI 描述。 */
        [[nodiscard]] nvrhi::ShaderDesc makeShaderDesc(const ShaderEntryDesc& entry);

    } // namespace detail

    /**
     * 按内置 `ShaderId` 加载并缓存 NvRHI shader 的 device/session 级资源库。
     *
     * 设备与 shader 目录必须覆盖本对象生命周期。该对象只能在拥有 NvRHI device 的渲染主线程使用；相同 ID
     * 在库生命周期内只读盘并调用一次 `createShader()`，返回 handle 与缓存共享所有权。
     */
    class ShaderLibrary final {
    public:
        /** 绑定设备和 CMake 生成的 SPIR-V 根目录。 */
        ShaderLibrary(nvrhi::IDevice& device, std::filesystem::path shaderDirectory);

        /**
         * 按稳定 ID 返回 shader；首次访问时从 Catalog 描述创建并缓存。
         *
         * @throws std::out_of_range ID 不属于内置 Catalog 时抛出。
         * @throws std::runtime_error 文件读取或设备创建失败时抛出。
         */
        [[nodiscard]] nvrhi::ShaderHandle load(ShaderId id);

    private:
        static constexpr std::size_t shaderCount = static_cast<std::size_t>(ShaderId::Count);

        nvrhi::IDevice& device_;
        std::filesystem::path shaderDirectory_;
        std::array<nvrhi::ShaderHandle, shaderCount> cache_{};
    };

} // namespace lumin::render

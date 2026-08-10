#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    /// 描述一个从磁盘加载并交给 NvRHI 创建的着色器模块。
    struct ShaderModuleDesc {
        /// 相对于 ShaderLibrary 根目录的二进制文件路径。
        std::filesystem::path fileName;
        /// 单一着色器阶段；不能使用阶段掩码。
        nvrhi::ShaderType shaderType = nvrhi::ShaderType::None;
        /// 二进制模块中的入口点名称。
        std::string entryPoint;
        /// 调试器中显示的名称；为空时使用 `fileName`。
        std::string debugName;
        /// NvAPI HLSL 扩展使用的 UAV 槽；`-1` 表示禁用。
        int hlslExtensionsUAV = -1;
    };

    namespace detail {

        /// 校验引擎模块描述并生成不持有外部字符串指针的 NvRHI 描述。
        [[nodiscard]] nvrhi::ShaderDesc makeShaderDesc(const ShaderModuleDesc& desc);

        /// 返回阶段是否是一个可独立创建的光线追踪着色器阶段。
        [[nodiscard]] bool isRayTracingShaderType(nvrhi::ShaderType shaderType) noexcept;

    } // namespace detail

    /// 从统一目录加载着色器二进制并创建 NvRHI 着色器对象。
    class ShaderLibrary {
    public:
        /// 创建着色器库；设备与目录必须在加载调用期间保持有效。
        ShaderLibrary(nvrhi::IDevice& device, std::filesystem::path shaderDirectory);

        /// 按完整描述加载模块；文件或描述无效、设备创建失败时抛出异常。
        [[nodiscard]] nvrhi::ShaderHandle loadModule(const ShaderModuleDesc& desc) const;

        /// 兼容现有调用的简化加载接口。
        [[nodiscard]] nvrhi::ShaderHandle loadModule(const std::filesystem::path& fileName,
                                                     nvrhi::ShaderType shaderType, std::string_view entryPoint) const;

        /// 加载计算着色器；`entryPoint` 必须非空。
        [[nodiscard]] nvrhi::ShaderHandle loadComputeModule(const std::filesystem::path& fileName,
                                                            std::string_view entryPoint) const;

        /// 加载单一光线追踪阶段；传入非 RT 阶段时抛出 `std::invalid_argument`。
        [[nodiscard]] nvrhi::ShaderHandle loadRayTracingModule(const std::filesystem::path& fileName,
                                                               nvrhi::ShaderType shaderType,
                                                               std::string_view entryPoint) const;

    private:
        nvrhi::IDevice& device_;
        std::filesystem::path shaderDirectory_;
    };

} // namespace lumin::render

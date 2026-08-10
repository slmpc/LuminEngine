#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <nvrhi/nvrhi.h>

namespace lumin::render {

    /// 描述一个使用动态渲染目标格式创建的图形流水线。
    struct GraphicsPipelineDesc {
        /// 顶点着色器。
        nvrhi::ShaderHandle vertexShader;
        /// 像素着色器。
        nvrhi::ShaderHandle fragmentShader;
        /// 可选的顶点输入布局；全屏三角形等无顶点输入流水线可为空。
        nvrhi::InputLayoutHandle inputLayout;
        /// 按 descriptor set 顺序排列的全局绑定布局。
        std::span<const nvrhi::BindingLayoutHandle> bindingLayouts;
        /// 动态渲染颜色附件格式。
        std::span<const nvrhi::Format> colorFormats;
        /// 深度附件格式；无深度附件时为 `UNKNOWN`。
        nvrhi::Format depthFormat = nvrhi::Format::UNKNOWN;
        /// 是否启用深度测试。
        bool depthTestEnable = true;
        /// 是否写入深度。
        bool depthWriteEnable = true;
        /// 深度比较函数。
        nvrhi::ComparisonFunc depthCompareOp = nvrhi::ComparisonFunc::Less;
        /// 光栅化剔除模式。
        nvrhi::RasterCullMode cullMode = nvrhi::RasterCullMode::Back;
        /// 是否启用深度偏移。
        bool depthBiasEnable = false;
        /// 固定深度偏移。
        int depthBias = 0;
        /// 斜率缩放深度偏移。
        float slopeScaledDepthBias = 0.0f;
        /// 多重采样数量。
        std::uint32_t sampleCount = 1;
    };

    /// 描述一个计算流水线。
    struct ComputePipelineDesc {
        /// 计算着色器，类型必须为 `nvrhi::ShaderType::Compute`。
        nvrhi::ShaderHandle computeShader;
        /// 按 descriptor set 顺序排列的全局绑定布局。
        std::span<const nvrhi::BindingLayoutHandle> bindingLayouts;
    };

    /// 描述光线追踪流水线中的 ray generation、miss 或 callable 导出。
    struct RayTracingPipelineShaderDesc {
        /// shader table 引用的稳定导出名；在整条流水线内必须唯一。
        std::string exportName;
        /// 对应的 NvRHI 着色器对象。
        nvrhi::ShaderHandle shader;
    };

    /// 描述光线追踪流水线中的一个 hit group。
    struct RayTracingHitGroupDesc {
        /// shader table 引用的稳定导出名；在整条流水线内必须唯一。
        std::string exportName;
        /// 可选的 closest-hit 着色器。
        nvrhi::ShaderHandle closestHitShader;
        /// 可选的 any-hit 着色器。
        nvrhi::ShaderHandle anyHitShader;
        /// 过程几何必须提供的 intersection 着色器。
        nvrhi::ShaderHandle intersectionShader;
        /// `true` 表示过程几何，`false` 表示三角形几何。
        bool proceduralPrimitive = false;
    };

    /// 描述一条 NvRHI 光线追踪流水线。
    struct RayTracingPipelineDesc {
        /// ray generation、miss 与 callable 导出集合。
        std::span<const RayTracingPipelineShaderDesc> shaders;
        /// hit group 导出集合。
        std::span<const RayTracingHitGroupDesc> hitGroups;
        /// 按 descriptor set 顺序排列的全局绑定布局。
        std::span<const nvrhi::BindingLayoutHandle> globalBindingLayouts;
        /// payload 的最大字节数；允许为零。
        std::uint32_t maxPayloadSize = 0;
        /// attribute 的最大字节数，三角形重心坐标通常需要 8 字节。
        std::uint32_t maxAttributeSize = sizeof(float) * 2;
        /// `traceRay` 的最大递归深度，必须大于零。
        std::uint32_t maxRecursionDepth = 1;
        /// NvAPI HLSL 扩展使用的 UAV 槽；`-1` 表示禁用。
        std::int32_t hlslExtensionsUAV = -1;
        /// 是否允许流水线使用 opacity micromap。
        bool allowOpacityMicromaps = false;
    };

    /// 描述 shader table 中一个可重复的 miss、hit group 或 callable 记录。
    struct RayTracingShaderTableEntryDesc {
        /// 流水线导出名。
        std::string exportName;
    };

    /// 描述一次完整的 shader table 构建。
    struct RayTracingShaderTableDesc {
        /// ray generation 导出名。
        std::string rayGenerationExport;
        /// 按 shader table 索引排列的 miss 记录。
        std::span<const RayTracingShaderTableEntryDesc> missShaders;
        /// 按实例 shader binding table 偏移约定排列的 hit group 记录。
        std::span<const RayTracingShaderTableEntryDesc> hitGroups;
        /// 按 shader table 索引排列的 callable 记录。
        std::span<const RayTracingShaderTableEntryDesc> callableShaders;
        /// 是否缓存 GPU shader table；适合较大且低频更新的表。
        bool cached = false;
        /// 缓存表的最大总记录数；启用缓存时不得小于实际记录数。
        std::uint32_t maxEntries = 0;
        /// 调试器中显示的名称。
        std::string debugName;
    };

    namespace detail {

        /// 校验引擎描述并转换为 NvRHI 计算流水线描述。
        [[nodiscard]] nvrhi::ComputePipelineDesc makeComputePipelineDesc(const ComputePipelineDesc& desc);

        /// 校验引擎描述并转换为 NvRHI 光线追踪流水线描述。
        [[nodiscard]] nvrhi::rt::PipelineDesc makeRayTracingPipelineDesc(const RayTracingPipelineDesc& desc);

        /// 校验 shader table 的容量配置并生成 NvRHI 描述。
        [[nodiscard]] nvrhi::rt::ShaderTableDesc makeRayTracingShaderTableDesc(const RayTracingShaderTableDesc& desc);

        /**
         * 校验 shader table 引用的导出是否存在且类型正确。
         *
         * @param pipelineDesc 已完成校验的 NvRHI 光线追踪流水线描述。
         * @param tableDesc 待验证的 shader table 描述。
         * @throws std::invalid_argument 导出缺失、类型错误或容量配置无效。
         */
        void validateRayTracingShaderTable(const nvrhi::rt::PipelineDesc& pipelineDesc,
                                           const RayTracingShaderTableDesc& tableDesc);

    } // namespace detail

    /// 集中创建图形、计算与光线追踪流水线，并统一执行前置校验和错误处理。
    class PipelineFactory {
    public:
        /// 使用指定 NvRHI 设备创建工厂；设备生命周期必须覆盖工厂生命周期。
        explicit PipelineFactory(nvrhi::IDevice& device);

        /// 创建图形流水线；描述无效或设备创建失败时抛出异常。
        [[nodiscard]] nvrhi::GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) const;

        /// 创建计算流水线；描述无效或设备创建失败时抛出异常。
        [[nodiscard]] nvrhi::ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) const;

        /// 创建光线追踪流水线；描述无效或设备创建失败时抛出异常。
        [[nodiscard]] nvrhi::rt::PipelineHandle createRayTracingPipeline(const RayTracingPipelineDesc& desc) const;

        /**
         * 从光线追踪流水线创建并填充 shader table。
         *
         * 同一个导出可以重复加入 miss、hit group 或 callable 区域。Vulkan 路径通过全局 GPU scene
         * 与实例索引选择每条记录的数据，不使用 NvRHI 当前 Vulkan 后端不支持的 local binding。
         *
         * @throws std::invalid_argument 流水线为空、导出不存在或描述无效。
         * @throws std::runtime_error NvRHI 无法创建或填充 shader table。
         */
        [[nodiscard]] nvrhi::rt::ShaderTableHandle
        createRayTracingShaderTable(const nvrhi::rt::PipelineHandle& pipeline,
                                    const RayTracingShaderTableDesc& desc) const;

    private:
        nvrhi::IDevice& device_;
    };

} // namespace lumin::render

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lumin::render {

    /// 控制 Ray Tracing 构建或运行阶段的启用策略。
    enum class RayTracingMode : std::uint8_t {
        /// 条件满足时启用，否则允许回退到 raster 路径。
        Auto,
        /// 强制要求启用；条件不满足时必须拒绝启动。
        On,
        /// 显式禁用，不启用 RT 扩展、Feature 或资源。
        Off,
    };

    /**
     * @brief 描述 Ray Tracing 在构建和运行阶段的双层策略。
     *
     * `build` 表示构建产物是否应包含 RT 实现，`runtime` 表示当前进程是否请求使用它。
     * 两者均为 `Auto` 时，设备不满足条件会产生可诊断的 raster fallback，而不是启动失败。
     */
    struct RayTracingPolicy {
        /// 构建阶段策略，由构建系统映射到最终产物能力。
        RayTracingMode build = RayTracingMode::Auto;

        /// 运行阶段策略，可由配置文件或命令行覆盖。
        RayTracingMode runtime = RayTracingMode::Auto;
    };

    /// 描述 Vulkan 设备可实际使用的 Ray Tracing 路径组合。
    enum class VulkanRayTracingTier : std::uint8_t {
        /// 设备只能运行 raster 路径，或 RT 已被策略禁用。
        RasterOnly,
        /// 仅可通过内联 `rayQuery` 发射射线。
        RayQuery,
        /// 可使用 Ray Tracing Pipeline，但未启用 `rayQuery`。
        RayTracingPipeline,
        /// 同时可使用 Ray Tracing Pipeline 与内联 `rayQuery`。
        RayTracingPipelineWithRayQuery,
    };

    /// 保存 Ray Tracing 相关 Vulkan 设备扩展的枚举结果。
    struct VulkanRayTracingExtensionSupport {
        /// 是否存在 `VK_KHR_acceleration_structure`。
        bool accelerationStructure = false;

        /// 是否存在 `VK_KHR_ray_tracing_pipeline`。
        bool rayTracingPipeline = false;

        /// 是否存在 `VK_KHR_deferred_host_operations`。
        bool deferredHostOperations = false;

        /// 是否存在可选的 `VK_KHR_ray_query`。
        bool rayQuery = false;

        /// 是否存在 NRD compute shader 所需的 `VK_KHR_compute_shader_derivatives`。
        bool computeShaderDerivatives = false;
    };

    /// 保存通过 `vkGetPhysicalDeviceFeatures2` 查询到的 RT Feature 位。
    struct VulkanRayTracingFeatureSupport {
        /// `VkPhysicalDeviceFeatures::shaderInt64`，SHARC 的 64 位空间哈希键需要该能力。
        bool shaderInt64 = false;

        /// `VkPhysicalDeviceVulkan12Features::shaderFloat16`，SHARC 的压缩辐射缓存需要该能力。
        bool shaderFloat16 = false;

        /// `VkPhysicalDeviceVulkan11Features::uniformAndStorageBuffer16BitAccess`。
        bool uniformAndStorageBuffer16BitAccess = false;

        /// `VkPhysicalDeviceVulkan11Features::storageBuffer16BitAccess`。
        bool storageBuffer16BitAccess = false;

        /// `VkPhysicalDeviceVulkan12Features::bufferDeviceAddress`。
        bool bufferDeviceAddress = false;

        /// `VkPhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing`。
        bool shaderStorageBufferArrayNonUniformIndexing = false;

        /// `VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound`。
        bool descriptorBindingPartiallyBound = false;

        /// `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure`。
        bool accelerationStructure = false;

        /// `VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTracingPipeline`。
        bool rayTracingPipeline = false;

        /// `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery`。
        bool rayQuery = false;

        /// `VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR::computeDerivativeGroupQuads`。
        bool computeDerivativeGroupQuads = false;
    };

    /**
     * @brief 保存一次 Vulkan RT 能力探测的纯数据输入。
     *
     * 该类型不包含 Vulkan 句柄，可在无 GPU、无 Vulkan loader 的单元测试中构造。
     */
    struct VulkanRayTracingDeviceProbe {
        /// 设备扩展支持情况。
        VulkanRayTracingExtensionSupport extensions;

        /// 设备 Feature 支持情况。
        VulkanRayTracingFeatureSupport features;

        /// `VkPhysicalDeviceRayTracingPipelinePropertiesKHR::maxRayRecursionDepth`。
        std::uint32_t maxRayRecursionDepth = 0;
    };

    /// 标识启用 Ray Tracing Pipeline 时尚未满足的 Vulkan 条件。
    enum class VulkanRayTracingRequirement : std::uint8_t {
        /// 缺少 `VK_KHR_acceleration_structure`。
        AccelerationStructureExtension,
        /// 缺少 `VK_KHR_ray_tracing_pipeline`。
        RayTracingPipelineExtension,
        /// 缺少 `VK_KHR_deferred_host_operations`。
        DeferredHostOperationsExtension,
        /// 缺少 NRD 所需的 `VK_KHR_compute_shader_derivatives`。
        ComputeShaderDerivativesExtension,
        /// `bufferDeviceAddress` Feature 未启用。
        BufferDeviceAddressFeature,
        /// `shaderStorageBufferArrayNonUniformIndexing` Feature 未启用。
        ShaderStorageBufferArrayNonUniformIndexingFeature,
        /// `descriptorBindingPartiallyBound` Feature 未启用。
        DescriptorBindingPartiallyBoundFeature,
        /// `accelerationStructure` Feature 未启用。
        AccelerationStructureFeature,
        /// `rayTracingPipeline` Feature 未启用。
        RayTracingPipelineFeature,
        /// `computeDerivativeGroupQuads` Feature 未启用。
        ComputeDerivativeGroupQuadsFeature,
        /// `maxRayRecursionDepth` 小于引擎要求的两层递归。
        RayRecursionDepth,
    };

    /**
     * @brief 保存 Vulkan RT 探测后的能力快照。
     *
     * `tier` 描述硬件可用路径；策略解析可能仍选择 `RasterOnly`，因此调用方不应仅凭本结构创建 RT 资源。
     */
    struct VulkanRayTracingSupport {
        /// 设备提供的最高可用路径组合。
        VulkanRayTracingTier tier = VulkanRayTracingTier::RasterOnly;

        /// buffer device address 是否可用于 RT 资源。
        bool bufferDeviceAddressUsable = false;

        /// acceleration structure 是否满足扩展和 Feature 的完整条件。
        bool accelerationStructureUsable = false;

        /// Ray Tracing Pipeline 是否满足全部必需条件。
        bool rayTracingPipelineUsable = false;

        /// 内联 Ray Query 是否满足全部必需条件。
        bool rayQueryUsable = false;

        /// 设备是否支持 SHARC shader 数据布局所需的 64 位整数和 16 位 storage buffer。
        bool sharcShaderStorageUsable = false;

        /// 设备报告的最大 Ray Tracing Pipeline 递归深度。
        std::uint32_t maxRayRecursionDepth = 0;

        /// Ray Tracing Pipeline 缺失条件，按稳定顺序排列。
        std::vector<VulkanRayTracingRequirement> missingPipelineRequirements;

        /// 返回设备是否具有完整的 Ray Tracing Pipeline 能力。
        [[nodiscard]] bool supportsRayTracingPipeline() const noexcept;

        /// 返回设备是否具有完整的内联 Ray Query 能力。
        [[nodiscard]] bool supportsRayQuery() const noexcept;

        /// 返回设备是否能在已启用的 RT 路径上运行 SHARC shader。
        [[nodiscard]] bool supportsSharcShaderStorage() const noexcept;
    };

    /// 描述策略解析的最终结果类别。
    enum class RayTracingDecisionStatus : std::uint8_t {
        /// 已启用 RT，逻辑设备必须启用决策要求的扩展和 Feature。
        Enabled,
        /// 根据显式 OFF 策略关闭 RT。
        Disabled,
        /// AUTO 条件不满足，允许继续使用 raster fallback。
        Fallback,
        /// ON 条件不满足，调用方必须拒绝启动或改用其他物理设备。
        Rejected,
    };

    /// 标识一条 Ray Tracing 策略诊断的原因类别。
    enum class RayTracingDiagnosticCode : std::uint8_t {
        /// 构建策略显式关闭 RT。
        DisabledByBuildPolicy,
        /// 运行策略显式关闭 RT。
        DisabledByRuntimePolicy,
        /// 构建 OFF 与运行 ON 互相冲突。
        ConflictingPolicies,
        /// 当前构建产物未包含 RT 实现。
        ImplementationUnavailable,
        /// 缺少必需的 Vulkan 设备扩展。
        MissingDeviceExtension,
        /// 缺少必需的 Vulkan Feature。
        MissingDeviceFeature,
        /// Vulkan 设备属性低于引擎要求。
        InsufficientDeviceProperty,
    };

    /// 保存一条可机器分类、可格式化输出的 Ray Tracing 诊断。
    struct RayTracingDiagnostic {
        /// 稳定的诊断类别。
        RayTracingDiagnosticCode code = RayTracingDiagnosticCode::ImplementationUnavailable;

        /// 缺失扩展、Feature、属性或策略冲突的具体标识。
        std::string subject;
    };

    /**
     * @brief 保存构建策略、运行策略和设备能力解析后的最终决策。
     *
     * 只有 `enabled()` 返回 `true` 时，调用方才可把 `enabledDeviceExtensions()` 返回的扩展加入逻辑设备，
     * 并创建 RT 资源。`Fallback` 与 `Disabled` 都必须保持 RT 资源未创建状态。
     */
    struct RayTracingDecision {
        /// 决策结果类别。
        RayTracingDecisionStatus status = RayTracingDecisionStatus::Disabled;

        /// 最终启用的路径；非 `Enabled` 状态始终为 `RasterOnly`。
        VulkanRayTracingTier tier = VulkanRayTracingTier::RasterOnly;

        /// 导致禁用、回退或拒绝的结构化原因。
        std::vector<RayTracingDiagnostic> diagnostics;

        /// 返回当前决策是否允许创建和使用 RT 资源。
        [[nodiscard]] bool enabled() const noexcept;

        /// 返回当前决策是否要求拒绝当前设备或终止启动。
        [[nodiscard]] bool rejected() const noexcept;

        /// 返回逻辑设备是否应启用可选的 `rayQuery`。
        [[nodiscard]] bool enablesRayQuery() const noexcept;
    };

    /// 从设备扩展名称列表提取 RT 扩展支持状态；比较区分大小写。
    [[nodiscard]] VulkanRayTracingExtensionSupport
    inspectVulkanRayTracingExtensions(std::span<const std::string_view> availableExtensions) noexcept;

    /// 根据扩展、Feature 和属性计算设备 RT tier 及缺失条件。
    [[nodiscard]] VulkanRayTracingSupport evaluateVulkanRayTracingSupport(const VulkanRayTracingDeviceProbe& probe);

    /**
     * @brief 解析构建、运行和设备能力，生成唯一的 RT 启用决策。
     *
     * @param policy 构建与运行策略。
     * @param implementationBuilt 当前构建产物是否实际包含 RT 后端实现。
     * @param deviceSupport 当前 Vulkan 物理设备的 RT 能力。
     * @throws std::invalid_argument 策略枚举值无效时抛出。
     */
    [[nodiscard]] RayTracingDecision resolveRayTracingPolicy(const RayTracingPolicy& policy, bool implementationBuilt,
                                                             const VulkanRayTracingSupport& deviceSupport);

    /**
     * @brief 返回决策要求启用的 Vulkan RT 设备扩展。
     *
     * 返回值不包含 `VK_KHR_swapchain` 等基础扩展。非启用决策返回空列表；字符串指向静态存储期常量。
     */
    [[nodiscard]] std::vector<std::string_view>
    enabledVulkanRayTracingDeviceExtensions(const RayTracingDecision& decision);

    /// 返回 Vulkan RT 缺失条件的稳定英文标识，用于日志与测试。
    [[nodiscard]] std::string_view vulkanRayTracingRequirementName(VulkanRayTracingRequirement requirement) noexcept;

    /// 返回 RT tier 的稳定英文标识，用于日志与配置诊断。
    [[nodiscard]] std::string_view vulkanRayTracingTierName(VulkanRayTracingTier tier) noexcept;

    /// 将结构化决策格式化为单行英文诊断，可直接写入日志或异常。
    [[nodiscard]] std::string formatRayTracingDecision(const RayTracingDecision& decision);

} // namespace lumin::render

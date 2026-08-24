#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lumin::render {

    /** 内置 shader 入口的稳定类型化标识。 */
    enum class ShaderId : std::uint16_t {
        DeferredVertex,
        DeferredFragment,
        GBufferVertex,
        GBufferFragment,
        GiCompositeCompute,
        HybridLightingCompositeCompute,
        ImGuiVertex,
        ImGuiFragment,
        PostProcessVertex,
        PostProcessFragment,
        RtDiRayGeneration,
        RtDiRadianceMiss,
        RtDiShadowMiss,
        RtDiClosestHit,
        RtDiNrdInputsCompute,
        SharcIndirectLightingRayGeneration,
        SharcIndirectLightingRadianceMiss,
        SharcIndirectLightingShadowMiss,
        SharcIndirectLightingClosestHit,
        ShadowVertex,
        SharcResolveCompute,
        SharcUpdateRayGeneration,
        SharcUpdateRadianceMiss,
        SharcUpdateShadowMiss,
        SharcUpdateClosestHit,
        SkyVertex,
        SkyFragment,
        TaaVertex,
        TaaFragment,
        AmbientOcclusionVertex,
        AmbientOcclusionFragment,
        BloomVertex,
        BloomFragment,
        AtmosphereAerialPerspective,
        AtmosphereMultiScattering,
        AtmosphereSkyView,
        AtmosphereTransmittance,
        Count,
    };

    /** 与 Slang/NvRHI 映射无关的单一 shader stage。 */
    enum class ShaderStage : std::uint8_t {
        Compute,
        Vertex,
        Fragment,
        RayGeneration,
        Miss,
        ClosestHit,
    };

    /** 构建 shader 所需的可选引擎能力。 */
    enum class ShaderFeature : std::uint8_t {
        RayTracing,
        Nrd,
        Sharc,
    };

    /** 反射 ABI 校验支持的资源绑定类型。 */
    enum class ShaderBindingKind : std::uint8_t {
        ConstantBuffer,
        StructuredBuffer,
        SampledImage,
        SampledImage3D,
        Sampler,
        StorageImage,
        StorageImage3D,
        AccelerationStructure,
        PushConstant,
    };

    /** 单个 descriptor 或 push-constant 绑定的期望 ABI。 */
    struct ShaderBindingDesc {
        /** Slang 反射资源名称。 */
        std::string name;
        /** 资源类型。 */
        ShaderBindingKind kind = ShaderBindingKind::ConstantBuffer;
        /** Descriptor set；push constant 不提供该值。 */
        std::optional<std::uint32_t> descriptorSet;
        /** Vulkan binding 或 push-constant register 索引。 */
        std::uint32_t binding = 0;
    };

    /** 单个 Slang 结构字段的固定布局。 */
    struct ShaderAbiFieldDesc {
        /** 字段名称。 */
        std::string name;
        /** 字段相对结构起始位置的字节偏移。 */
        std::uint32_t offset = 0;
        /** 字段占用字节数。 */
        std::uint32_t size = 0;
    };

    /** CPU/GPU 共享结构的固定布局。 */
    struct ShaderAbiStructDesc {
        /** Slang 结构名称。 */
        std::string name;
        /** 结构总字节数。 */
        std::uint32_t size = 0;
        /** 需要锁定的字段布局。 */
        std::vector<ShaderAbiFieldDesc> fields;
    };

    /** 单个可加载 shader 入口及其完整构建/反射契约。 */
    struct ShaderEntryDesc {
        /** 稳定运行时 ID。 */
        ShaderId id = ShaderId::Count;
        /** 构建与诊断使用的唯一名称。 */
        std::string name;
        /** 相对 shader 源码根目录的 Slang 文件。 */
        std::string source;
        /** Slang 入口函数名称。 */
        std::string entryPoint;
        /** 单一 shader stage。 */
        ShaderStage stage = ShaderStage::Compute;
        /** 相对构建输出目录的 SPIR-V 路径。 */
        std::string output;
        /** 相对构建输出目录的 reflection JSON 路径。 */
        std::string reflection;
        /** 相对构建输出目录的 depfile 路径。 */
        std::string depfile;
        /** 启用入口所需的构建 feature。 */
        std::vector<ShaderFeature> requirements;
        /** 传给 Slang 的显式 capability。 */
        std::vector<std::string> capabilities;
        /** 相对 shader 根目录的附加 import/include 搜索目录。 */
        std::vector<std::string> includeDirectories;
        /** 入口专用预处理 define。 */
        std::vector<std::string> defines;
        /** 入口专用 `slangc` 参数。 */
        std::vector<std::string> options;
        /** 反射必须精确匹配的资源 binding。 */
        std::vector<ShaderBindingDesc> bindings;
        /** 反射必须固定布局的 ABI 结构名称。 */
        std::vector<std::string> abiStructs;
    };

    /** 所有内置 shader 的编译默认值、ABI 与入口表。 */
    struct ShaderCatalog {
        /** Slang 输出目标。 */
        std::string target = "spirv";
        /** Slang 编译 profile。 */
        std::string profile = "spirv_1_5";
        /** 优化等级，不包含 `-O` 前缀。 */
        std::string optimization = "3";
        /** 矩阵布局参数。 */
        std::string matrixLayout = "column-major";
        /** 警告升级错误策略。 */
        std::string warningsAsErrors = "all";
        /** 所有入口共享的 import/include 搜索目录。 */
        std::vector<std::string> includeDirectories{"include"};
        /** 全局 CPU/GPU ABI 结构表。 */
        std::vector<ShaderAbiStructDesc> abiStructs;
        /** 按稳定 ID 注册的入口表。 */
        std::vector<ShaderEntryDesc> entries;

        /** 按稳定 ID 返回入口；ID 不在目录中时抛出 `std::out_of_range`。 */
        [[nodiscard]] const ShaderEntryDesc& entry(ShaderId id) const;
    };

    /** 在单个源模块上复用构建条件与 ABI 契约的链式配置器。 */
    class ShaderModuleBuilder final {
    public:
        /** 为模块追加 feature 条件。 */
        ShaderModuleBuilder& requireFeatures(std::initializer_list<ShaderFeature> features);
        /** 为模块追加 Slang capability。 */
        ShaderModuleBuilder& capabilities(std::initializer_list<std::string_view> capabilities);
        /** 为模块追加 include/import 搜索目录。 */
        ShaderModuleBuilder& includeDirectories(std::initializer_list<std::string_view> directories);
        /** 为模块追加反射 binding 契约。 */
        ShaderModuleBuilder& bindings(std::initializer_list<ShaderBindingDesc> bindings);
        /** 为模块追加需要固定布局的 ABI 结构。 */
        ShaderModuleBuilder& abiStructs(std::initializer_list<std::string_view> names);
        /**
         * 追加一个入口；`artifactStem` 自动派生 `.spv`、reflection JSON 与 depfile 路径。
         */
        ShaderModuleBuilder& entry(ShaderId id, std::string_view name, std::string_view entryPoint, ShaderStage stage,
                                   std::string_view artifactStem,
                                   std::initializer_list<std::string_view> extraCapabilities = {});

    private:
        friend class ShaderCatalogBuilder;

        explicit ShaderModuleBuilder(std::string source);

        std::string source_;
        std::vector<ShaderFeature> requirements_;
        std::vector<std::string> capabilities_;
        std::vector<std::string> includeDirectories_;
        std::vector<ShaderBindingDesc> bindings_;
        std::vector<std::string> abiStructs_;
        std::vector<ShaderEntryDesc> entries_;
    };

    /** 构造并完整校验不可变 ShaderCatalog 的集中 Builder。 */
    class ShaderCatalogBuilder final {
    public:
        /** 注册一个全局 ABI 结构。 */
        ShaderCatalogBuilder& abiStruct(std::string_view name, std::uint32_t size,
                                        std::initializer_list<ShaderAbiFieldDesc> fields);
        /** 开始配置一个 Slang 源模块；返回引用在下一次 `module()` 前有效。 */
        ShaderModuleBuilder& module(std::string_view source);
        /** 校验 ID、名称、输出和 ABI 唯一性并生成最终目录。 */
        [[nodiscard]] ShaderCatalog build() &&;

    private:
        ShaderCatalog catalog_;
        std::vector<ShaderModuleBuilder> modules_;
    };

    /** 返回进程内唯一的内置 shader 目录。 */
    [[nodiscard]] const ShaderCatalog& builtinShaderCatalog();

    /** 返回与构建工具兼容的 stage 字符串。 */
    [[nodiscard]] std::string_view toString(ShaderStage stage) noexcept;
    /** 返回与构建工具兼容的 feature 字符串。 */
    [[nodiscard]] std::string_view toString(ShaderFeature feature) noexcept;
    /** 返回与 ABI validator 兼容的 binding kind 字符串。 */
    [[nodiscard]] std::string_view toString(ShaderBindingKind kind) noexcept;

} // namespace lumin::render

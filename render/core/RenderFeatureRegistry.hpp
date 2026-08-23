#pragma once

#include "render/core/RenderFeaturePipeline.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumin::render::core {

    /**
     * @brief 使用显式创建上下文构造一个尚未初始化的 Feature 实例。
     *
     * Factory 不得依赖全局 Renderer、活动场景或 Editor 状态；返回实例随后由 `RenderPipelineInstance`
     * 调用 `initialize()`。返回空指针会被 registry 拒绝。
     */
    using RenderFeatureFactory = std::function<std::unique_ptr<IRenderFeature>(const FeatureCreateContext& context)>;

    /**
     * @brief 显式保存静态链接 Feature 的描述符和工厂。
     *
     * Registry 不拥有任何 GPU 资源，也不会依赖全局静态初始化。注册完成后可在渲染主线程上解析 recipe
     *
     * 并创建独立实例；调用方必须保证并发读取期间不再注册。
     */
    class RenderFeatureRegistry final {
    public:
        /**
         * @brief 注册一个静态 Feature 模块。
         * @throws std::invalid_argument 工厂为空、标识重复或数据契约自相矛盾时抛出。
         */
        void registerFeature(FeatureDescriptor descriptor, RenderFeatureFactory factory);

        /// 返回指定 Feature 是否已注册。
        [[nodiscard]] bool contains(const FeatureId& id) const noexcept;

        /// 返回注册描述符的只读指针；不存在时返回 `nullptr`。
        [[nodiscard]] const FeatureDescriptor* descriptor(const FeatureId& id) const noexcept;

        /**
         * @brief 调用对应工厂创建尚未初始化的 Feature。
         * @throws std::out_of_range Feature 未注册时抛出。
         * @throws std::runtime_error 工厂返回空指针或不匹配描述符时抛出。
         */
        [[nodiscard]] std::unique_ptr<IRenderFeature> create(const FeatureId& id,
                                                             const FeatureCreateContext& context) const;

        /// 返回已注册 Feature 数量。
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        struct Entry {
            FeatureDescriptor descriptor;
            RenderFeatureFactory factory;
        };

        std::vector<Entry> entries_;
        std::unordered_map<FeatureId, std::size_t, FeatureIdHash> indices_;
    };

    /** 描述一条渲染路径选择的 Feature 集合及由协调器注入的帧数据。 */
    struct RenderPipelineRecipe {
        /// recipe 的稳定诊断标识。
        std::string id;

        /// 被选中的 Feature；解析器会根据数据依赖重新排序。
        std::vector<FeatureId> features;

        /// 由 Runtime 在 Feature 执行前写入 blackboard 的数据契约。
        std::vector<FrameDataContract> externalInputs;
    };

    /// 描述 recipe 中单个 Feature 的能力及数据依赖解析结果。
    enum class RecipeFeatureActivation {
        /// Feature 已启用并会被实例化。
        Enabled,
        /// Feature 因设备缺少必需能力而被禁用。
        DisabledMissingCapabilities,
        /// Feature 因显式依赖不可用而被禁用。
        DisabledDependency,
        /// Feature 因必需输入没有可用 producer 而被禁用。
        DisabledInput,
    };

    /// 保存 recipe 中一个 Feature 的激活状态和首个失败原因。
    struct ResolvedRecipeFeature {
        /// 使用稳定 Feature 标识构造默认启用的解析结果。
        explicit ResolvedRecipeFeature(FeatureId featureId);

        /// Feature 的稳定标识。
        FeatureId id;
        /// 最终激活状态。
        RecipeFeatureActivation activation = RecipeFeatureActivation::Enabled;
        /// 导致禁用的缺失能力集合。
        CapabilitySet missingCapabilities;
        /// 导致禁用的首个显式依赖。
        std::optional<FeatureId> unavailableDependency;
        /// 导致禁用的首个 typed input。
        std::optional<FrameDataContract> unavailableInput;

        /// 返回 Feature 是否会进入最终执行顺序。
        [[nodiscard]] bool enabled() const noexcept;
    };

    /**
     * @brief 由数据生产者到消费者排序后的不可变 recipe 解析结果。
     *
     * 结果按值拥有全部标识和诊断，不引用 registry 或 recipe，可安全传给候选 PipelineInstance。
     */
    class ResolvedRenderPipeline final {
    public:
        /// 返回 recipe 的稳定标识。
        [[nodiscard]] const std::string& id() const noexcept;

        /// 返回包含禁用诊断在内的全部 Feature 状态。
        [[nodiscard]] std::span<const ResolvedRecipeFeature> features() const noexcept;

        /// 返回只含已启用 Feature 的确定性 DAG 执行顺序。
        [[nodiscard]] std::span<const FeatureId> executionOrder() const noexcept;

        /// 查找 Feature 解析状态；recipe 未选择该 Feature 时返回 `nullptr`。
        [[nodiscard]] const ResolvedRecipeFeature* find(const FeatureId& id) const noexcept;

    private:
        friend class RenderPipelineRecipeResolver;

        std::string id_;
        std::vector<ResolvedRecipeFeature> features_;
        std::vector<FeatureId> executionOrder_;
    };

    /** 根据能力、显式依赖及 typed 数据输入输出解析可执行 Feature DAG。 */
    class RenderPipelineRecipeResolver final {
    public:
        /**
         * @brief 解析 recipe 并生成确定性执行顺序。
         * @throws std::invalid_argument 注册、输入输出、历史所有权或 DAG 结构无效时抛出。
         * @throws std::runtime_error 严格 Feature 的必需条件不可满足时抛出。
         */
        [[nodiscard]] static ResolvedRenderPipeline resolve(const RenderFeatureRegistry& registry,
                                                            const RenderPipelineRecipe& recipe,
                                                            const RenderDeviceCapabilities& capabilities);
    };

} // namespace lumin::render::core

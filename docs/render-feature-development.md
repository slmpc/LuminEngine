# Render Feature 开发指南

## 选择所属模块

Feature 必须加入拥有其 GPU 资源的真实 target，而不是 `Lumin::RenderRuntime`：

- G-buffer、阴影和 raster scene：`Lumin::RasterFeatures`。
- GPU buffer、BLAS/TLAS 和 descriptor array：`Lumin::GpuScene`。
- RTDI、SHARC update/indirect、NRD 和 GI composite：`Lumin::GiFeatures`。
- 大气 LUT：`Lumin::Atmosphere`。
- TAA、tone mapping 等后处理：`Lumin::PostFxFeatures`。
- Viewport、UI 和交换链输出：`Lumin::RenderPresentation`。

新增源码后只修改所属 target 的源文件列表。`Lumin::RenderRuntime` 只能依赖 `RenderCore`、`RenderRhi` 和
`VulkanBackend`，不得包含具体 Feature、默认 recipe 或 Editor 头。

## 实现生命周期

Feature 实现 `core::IRenderFeature`，生命周期固定为：

```text
factory -> initialize -> [onRenderExtentChanged] -> addPasses
        -> onFrameSubmitted | onFrameDiscarded -> ... -> shutdown
```

`initialize()` 只消费 `FeatureCreateContext` 中显式注入的 NvRHI device、`GpuResourceManager`、`ShaderLibrary`、
`PipelineFactory`、设备能力和帧槽数量。上下文及服务指针均为非拥有且只能在调用期间使用；Feature 必须自己保存创建出的
resource、binding、pipeline 和 history handle。初始化中途失败由 `RenderPipelineInstance` 逆序调用 `shutdown()`。

`addPasses()` 只构建当前 `FrameGraph`。它从 typed blackboard 读取 descriptor 声明的输入，发布 descriptor 声明的输出；
不得在这里推进跨帧历史。GPU 数据必须同时包含物理 NvRHI handle、`FrameGraphResourceHandle`、format、extent 和 ready
pass。导入持久资源时使用本帧唯一的 `FrameGraphResourceImporter`，同一物理资源不得二次 import。

只有 `onFrameSubmitted()` 可以发布候选状态。`onFrameDiscarded()` 必须撤销录制失败、提交失败或管线替换留下的候选；
两个回调都不得抛出异常。尺寸相关资源只在 `onRenderExtentChanged()` 的安全帧边界重建，稳态 `addPasses()` 不得创建资源
或调用 `waitIdle()`。

## 注册与 recipe

稳定 ID、默认 descriptor 和 Raster/Hybrid recipe 位于 `render/pipelines/DefaultRenderPipelines.*`。descriptor 至少声明：

- `requiredCapabilities` 与能力不足策略。
- `requiredInputs`、`optionalInputs` 和 `outputs`。
- 独占的 `historyDomains`。
- 仅用于无数据副作用边界的 `before/after` 约束。

默认静态模块在 `render/pipelines/default/DefaultFeatureRegistry.cpp` 显式注册。Registry 会拒绝重复 ID，resolver 会拒绝
缺失 producer、重复 producer、环和重复历史 owner。添加新 Feature 不得修改 `Renderer`、`VulkanContext` 或其他
Feature；正常改动范围应是所属模块、显式注册、recipe、可选设置 adapter 和测试。

每个注册项必须创建一个语义明确的具体 `IRenderFeature` 类型，并由该类型直接实现 `addPasses`、提交、丢弃、尺寸变化和
关闭阶段中适用的生命周期。禁止用保存 `std::function` 的万能 Feature 壳把不同模块重新集中到一组中央回调中。

## 验证清单

- 为 descriptor、能力降级、初始化回滚和提交/丢弃顺序添加 CPU 测试。
- 独立构建所属 Feature target 与 `lumin_render_pipelines`。
- 修改 shader 时运行 shader ABI 测试。
- 修改资源状态或历史时运行完整 CTest，并在 Vulkan validation 和 RenderDoc 中检查实际帧。
- 同一变更更新本指南或 `rendering-architecture.md` 中受影响的契约。

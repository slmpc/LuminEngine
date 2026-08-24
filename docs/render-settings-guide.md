# Render Settings 适配指南

## 数据流

渲染设置的数据流固定为：

```text
Editor panel -> RenderSettingsPanelAdapter -> RenderSettingsStore
             -> immutable RenderSettingsSnapshot -> RenderFramePacket -> Feature
```

Feature 不包含 ImGui 代码，也不读取 Editor 的聚合 `RenderSettings` 对象。渲染主线程 adapter 负责把面板值写入 typed
store；packet 只携带不可变快照。同步 Renderer 相对最近一次成功 GPU submit 的快照计算 diff，跳过或提交失败的帧不会
成为设置或历史基线。

## 注册 schema

每个可配置 Feature 使用自己的稳定 `FeatureId` 和设置类型，在
`pipelines::registerDefaultRenderSettings()` 中注册默认值、校验器和 diff 分类。设置类型需要可按值复制和比较，校验器必须在
主线程发布快照前拒绝 NaN、越界值和不支持的组合。

变化影响使用 `SettingsChangeImpact`：

- `HotUpdate`：只更新 uniform 或 CPU 标量，不重建资源，不重置历史。
- `HistoryReset`：资源拓扑不变，但一个或多个 `HistoryDomain` 的样本不能继续使用。
- `PipelineRecompose`：Feature 集合、能力要求或 DAG 结构变化；在渲染主线程帧边界事务式创建候选实例。
- `ResourceRecreate`：格式、尺寸、descriptor 容量或 shader 变体使 Feature 自有资源必须重建。

影响可以组合。需要历史处理时同时返回具体 `FrameChangeSet`，不要在 session 或 Feature 内再次写一套布尔失效规则。

## Editor adapter

`RenderSettingsPanelAdapter` 是 Editor 与 typed store 的唯一适配层。新增设置时：

1. 在所属 Feature 的设置类型中增加字段和中文 Doxygen 注释。
2. 更新默认 schema 的默认值、校验器和 diff 分类。
3. 在 adapter 的聚合读写中映射该字段。
4. 在 Editor panel 调用 adapter 暴露的编辑值，不向 Feature 传递 ImGui 或回调。
5. 添加默认值、非法值、snapshot 不可变性和影响分类测试。

公开设置、实际 UI、默认 recipe 和文档必须在同一提交中保持一致。删除或重命名字段时，不得保留已经失效的面板说明或
架构描述。

## 内置后处理设置

`postfx.bloom` 使用独立的 `BloomSettings` schema，`postfx.tonemap` 使用 `ToneMappingSettings`。当前后处理项全部归类为
`HotUpdate`：它们只改变下一帧的 pass 分支、push constants 或 uniform，不触发 pipeline 重组、资源重建或 TAA 历史失效。

Editor 的 `Render / GI` 面板提供以下设置：

- `AgX`：默认开启；关闭后使用 ACES Filmic。
- `Exposure`：保留原有线性倍率语义，默认 `1.0`；自动曝光开启时仍作为最终倍率参与合成。
- `Auto exposure`：默认开启；在 GPU 上对 Bloom 后的 scene-linear HDR 画面测光。
- `Compensation EV`：默认 `0 EV`，范围 `[-8, 8]`，叠加到自动曝光结果。
- `EV range`：默认 `[-3, 10] EV`，完整 schema 范围 `[-16, 16]`，最小值与最大值至少相差 `0.1 EV`。
- `Brighten speed` / `Darken speed`：默认 `3.0/s` 与 `1.0/s`，按真实成功提交帧间隔进行指数适应。
- `Bloom`：默认开启；关闭时跳过多级滤波并复制 TAA HDR 输出。
- `Intensity`：默认 `0.08`，面板范围 `[0, 0.5]`。
- `Threshold`：默认 `1.0`，面板范围 `[0, 8]`。
- `Soft knee`：默认 `0.5`，范围 `[0, 1]`。
- `Radius`：默认 `1.0`，范围 `[0.5, 4]`。

项目保存时，这些值写入场景 JSON 的 `projectSettings.render`：`agx`、`autoExposure`、
`exposureCompensationEv`、`minimumExposureEv`、`maximumExposureEv`、`adaptationSpeedUp`、
`adaptationSpeedDown`、`bloom`、`bloomIntensity`、`bloomThreshold`、`bloomSoftKnee` 和 `bloomRadius`。旧项目缺失字段时
使用上述默认值；加载后会将越界的有限数值
归一化到 schema 支持范围，非有限值由 typed schema 拒绝。Editor 修改设置后仍通过 `ProjectSession` 的
dirty/save 流程持久化，不直接写项目文件。

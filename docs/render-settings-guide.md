# Render Settings 适配指南

## 数据流

渲染设置的数据流固定为：

```text
Editor panel -> RenderSettingsPanelAdapter -> RenderSettingsStore
             -> immutable RenderSettingsSnapshot -> RenderFramePacket -> Feature
```

Feature 不包含 ImGui 代码，也不读取 Editor 的聚合 `RenderSettings` 对象。主线程 adapter 负责把面板值写入 typed store；
packet 只携带不可变快照。渲染线程相对最近一次成功 GPU submit 的快照计算 diff，被 latest-wins mailbox 替换的 packet 不会
成为设置或历史基线。

## 注册 schema

每个可配置 Feature 使用自己的稳定 `FeatureId` 和设置类型，在
`pipelines::registerDefaultRenderSettings()` 中注册默认值、校验器和 diff 分类。设置类型需要可按值复制和比较，校验器必须在
主线程发布快照前拒绝 NaN、越界值和不支持的组合。

变化影响使用 `SettingsChangeImpact`：

- `HotUpdate`：只更新 uniform 或 CPU 标量，不重建资源，不重置历史。
- `HistoryReset`：资源拓扑不变，但一个或多个 `HistoryDomain` 的样本不能继续使用。
- `PipelineRecompose`：Feature 集合、能力要求或 DAG 结构变化；在渲染线程帧边界事务式创建候选实例。
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

# SHARC 辐亮度缓存

Lumin Engine 的 SHARC 集成使用固定版本 `1.6.5`，shader 直接包含
`thirdparty/sharc/include/SharcCommon.h`。引擎只提供资源绑定、稀疏光线追踪、FrameGraph 同步和提交事务，
不复制或改写 vendor 算法实现。

## 帧内顺序

每帧按以下顺序执行：

1. `sharc-clear` 清零四个统计 counter；首次、camera cut、拓扑或 geometry 变化时同时清零全部 cache buffer。
2. `sharc-sparse-update` 为每个 `5x5` tile 选择一个随成功提交序号变化的像素，追踪一条 diffuse path，
   并通过 `SharcInit`、`SharcUpdateHit`、`SharcSetThroughput` 和 `SharcUpdateMiss` 写入 cache。
3. `sharc-resolve-evict` 对所有 entry 调用 vendor `SharcResolveEntry`。该调用合并新旧样本、清空本帧
   accumulation，并在 stale window 到期后驱逐 hash entry。
4. `gi-ray-trace` 在二次表面调用 `SharcGetCachedRadiance`。命中时提前使用 cache radiance；未命中时保留
   直接光照 fallback，ray miss 则采样大气系统生成的同一张 sky-view LUT。
5. `sharc-statistics-readback` 在 query 完成后复制统计 buffer。只有对应 frame-slot fence 完成后才能映射。

所有 buffer 和 sky-view LUT 的访问状态都通过同一个 `FrameGraphResourceHandle` 声明。shader dispatch 内没有
手写 barrier；`UnorderedAccess` 的 write/read 和 write/write hazard 由 `FrameGraph` 生成内存依赖。

## 资源 ABI

默认容量是 `2^20` 个 entry，可通过 `SharcRadianceCacheConfig::capacity` 调整，但必须保持为不小于 16 的
2 次幂。每个 entry 的固定布局如下：

| Buffer | 每 entry 字节 | 用途 |
|---|---:|---|
| hash entries | 8 | 64-bit spatial hash key |
| accumulation | 16 | 本帧整数辐亮度与样本计数 |
| resolved | 16 | 跨帧 FP16 辐亮度、样本数和 stale 状态 |
| lock | 4 | 32-bit insertion lock |

adapter 固定以下宏：

```text
SHARC_ENABLE_64_BIT_ATOMICS=0
SHARC_USE_FP16=0
```

因此 Vulkan 设备不需要 `shaderBufferInt64Atomics`，但 hash 运算仍需要 `shaderInt64`。`SharcPackedData` 的
`radianceData` 在 vendor ABI 中始终是 `float16_t4`，与 `SHARC_USE_FP16` 无关，所以仍要求
`shaderFloat16`、`uniformAndStorageBuffer16BitAccess` 与 `storageBuffer16BitAccess`。前两者覆盖 Slang
声明的 `UniformAndStorageBuffer16BitAccess` capability，后者覆盖实际 `StorageBuffer` 变量中的 16-bit 元素；
禁用 64-bit atomic 时 lock buffer 是强制资源，不能省略。

## 时序失效

`SharcHistoryTracker` 只在 submit 成功后发布候选状态。`discardPendingFrame()` 不改变成功提交序号、上一帧
相机位置或响应窗口，因此失败帧可用相同 change set 重试。

| 变化 | 策略 |
|---|---|
| 首帧、camera cut、拓扑、geometry | `FullReset`，清空 hash/accumulation/resolved/lock |
| material、灯光、大气 | `ResponsiveDecay`，临时把 accumulation window 缩短到 4 帧 |
| 无变化 | `Preserve`，默认最多累积 64 帧，并由 stale counter 逐步驱逐旧 entry |

响应窗口按成功提交帧计数；录制失败或 submit 失败不会消耗窗口。

## 统计契约

GPU statistics buffer 是四个连续 `uint32_t`：

| 索引 | 字段 | 含义 |
|---:|---|---|
| 0 | `queryHitCount` | query 成功使用 cache 的二次表面数 |
| 1 | `updateCount` | sparse update 成功写入或复用的 entry 数 |
| 2 | `overflowCount` | hash bucket 探测窗口耗尽的次数 |
| 3 | `occupancyCount` | resolve/evict 后仍有效的 entry 数 |

宿主在复用 frame slot 且其 fence 已完成后调用 `readbackStatistics(slot, true)`。在该 slot 尚无成功提交的
readback 时返回 `std::nullopt`。

## Shader 构建接入

manifest 的 include path 需要加入：

```text
../thirdparty/sharc/include
../thirdparty/nrd/Shaders
```

需要注册以下 entry：

| 源文件 | entry | stage | 建议输出 |
|---|---|---|---|
| `SharcUpdate.slang` | `sharcUpdateRayGenerationMain` | raygeneration | `SharcUpdate.rgen.spv` |
| `SharcUpdate.slang` | `sharcUpdateRadianceMissMain` | miss | `SharcUpdate.radiance.rmiss.spv` |
| `SharcUpdate.slang` | `sharcUpdateShadowMissMain` | miss | `SharcUpdate.shadow.rmiss.spv` |
| `SharcUpdate.slang` | `sharcUpdateClosestHitMain` | closesthit | `SharcUpdate.rchit.spv` |
| `SharcResolve.slang` | `sharcResolveMain` | compute | `SharcResolve.comp.spv` |
| `RtGiSharc.slang` | `closestHitMain` | closesthit | `RtGiSharc.rchit.spv` |

`RtGiSharc.slang` 是只定义 `LUMIN_ENABLE_SHARC=1` 后包含主 RT GI 源码的薄 wrapper，因此无需给 manifest
增加 per-entry define 功能。全部 RT entry 使用项目现有 `spvRayTracingKHR` capability 集；resolve 使用普通
`spirv_1_5` compute 配置。`slangc` 的 depfile 会跟踪 vendor headers、NRD header 和公共 Slang include。

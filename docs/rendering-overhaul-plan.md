# Lumin Engine 渲染架构与混合 GI 执行计划

## Goal

将现有单体式延迟渲染流程迁移为可组合的 Feature Pipeline，并在保持 Vulkan 1.3 dynamic rendering、
`FrameGraph` 独占运行时 barrier、帧槽 fence 约束的前提下，实现以下可运行能力：

- 传统 Blinn-Phong 直接光照，并保留现有 PBR 路径作为可选 shading model。
- Vulkan Ray Tracing 场景、SHARC 世界空间辐射缓存和 NRD 时空去噪组成的混合 GI。
- 可供 raster sky、RT miss shader 和 GI 共用的大气 LUT 系统。
- 面向公共接口的简体中文 Doxygen，以及覆盖生命周期、能力降级和历史失效的中文文档。

本计划允许 breaking change。每个阶段必须先满足对应验证门槛，才推进下一个阶段。

## 当前执行批次（2026-08-10）

- [x] 建立持续 Goal，并将本文件作为唯一执行计划。
- [x] 完成渲染目录拆分、Feature Pipeline 基础、GPU Scene、RT/NRD/SHARC adapter 与大气参数模型的首轮实现。
- [x] 固定 NRD、SHARC submodule，并建立按 `requires` 过滤 shader 和源码的 feature gate。
- [x] 收口 SHARC indirect/query/update/resolve 与共享 atmosphere set 的 shader manifest/ABI。
- [x] 通过 shader ABI、GI composite、SHARC indirect、NRD、SHARC 测试及 RT/NRD/SHARC 开关矩阵。
- [x] 审查实际 Vulkan 生命周期、descriptor、SBT、history 与降级路径，并修正集成缺口。
- [x] 已落地现有大气 LUT 与共享绑定；按最新范围不再追加 raster/RT 画面对照工作。
- [x] 完成 Debug/Release 全量构建、CTest、sandbox 与 Vulkan validation 验证。
- [x] 按用户最新要求冻结大气扩展范围；GI 完成后结束，不再把大气画面对照作为本 Goal 门槛。

此清单记录当前连续执行状态；下方阶段清单保留完整交付范围和各阶段验证门槛。

## 本次完成记录

- Debug 与 Release 均通过 `35/35` CTest；完整 Hybrid GI 配置的 shader ABI 为 `30/30`。
- RTX 4060 上的 Vulkan 1.3、RT pipeline/ray query 与 SHARC shader storage 三项硬件探针均实际通过。
- Debug sandbox 以默认 `Auto` 模式持续运行超过 60 秒，Vulkan validation 无错误，RT + SHARC + NRD
  每帧事务未再出现资源耗尽。
- `LUMIN_RAY_TRACING=OFF`、`LUMIN_ENABLE_SHARC=OFF`、`LUMIN_ENABLE_NRD=OFF` 三种独立构建图均成功；
  对应 shader ABI 分别启用 `19/30`、`24/30`、`24/30` entries。
- GPU Scene 回归测试覆盖两个 frame slot 的首次追赶、场景变更后的再次追赶，以及每阶段 1000 个稳定帧零新增
  buffer/BLAS/TLAS 分配。

## 固定约束

- `VulkanContext` 是唯一原生 Vulkan 平台边界；其他渲染模块优先使用 NvRHI。
- 图形通道继续使用 Vulkan 1.3 dynamic rendering，不引入 `VkRenderPass` 或原生 framebuffer。
- 运行时资源状态和 barrier 只由 `FrameGraph` 声明和执行。
- 帧槽 CPU/GPU 资源只在 `VulkanContext::beginFrame` 等待对应 fence 后更新。
- TAA、NRD、SHARC 和 Atmosphere LUT 分域管理历史；仅在成功提交后推进已提交帧状态。
- 外部 SDK 使用固定 commit 的 git submodule，不在 CMake configure 阶段联网下载。
- 用户可见文档使用简体中文；代码标识符、API 名称和命令保持英文。

## 依赖基线

| 依赖 | 固定版本 | Commit | 用途 |
| --- | --- | --- | --- |
| SHARC | `v1.6.5.0` | `0b9f58bbc8c41736042d4da964830a247e424a00` | 世界空间辐射缓存 |
| NRD | `v4.17.3` | `792eff196afdd350fd9c3f862119017ccb438a0e` | REBLUR/RELAX 时空去噪 |

引入 submodule 前需记录许可证、所需 shader include、编译宏和上游升级方式。SHARC 与 NRD 必须位于
`thirdparty/` 下，并通过 `Lumin::Sharc`、`Lumin::Nrd` 隔离，vendor ABI 不进入引擎公共头。

## 大气参考边界

- 大气 LUT 的 pass 划分、纹理参数化、更新依赖和数值稳定策略可参考本地 `Alpha-Piscium` shaderpack。
- 只提取可验证的算法设计与数据流；Lumin Engine 使用独立的 Slang 实现和 `FrameGraph` 资源声明，
  不复制受许可证限制的源码、资源或项目专用绑定约定。
- 在实现前记录参考文件、许可证结论、坐标系和单位转换；无法确认授权的内容仅作为行为对照。

## 阶段与状态

### 本轮架构重构执行记录（2026-08-11）

- [x] 将 CPU 侧源码移动到 `core/`，GPU、平台和 Editor 源码移动到 `render/`，组合宿主移动到 `application/`。
- [x] 以 `Lumin::Core`、`Lumin::Render` 和 `Lumin::Application` 固定依赖方向，旧 target 名仅保留 alias。
- [x] 删除未使用的小型 CMake 模块；根、Core、Render、Shaders、Sandbox、Tests 各自只保留一个入口。
- [x] 用 C++ `ShaderCatalogBuilder` 生成 build-tree manifest、custom commands 和 ABI 校验输入，删除手写 shader JSON。
- [x] 完成 Debug 全量构建、35 项 CTest，以及 `LUMIN_RAY_TRACING=OFF` 的 configure/Core 构建验证。

### 0. 基线与审计

- [x] 完成现有模块、场景 revision、shader ABI、同步和构建测试审计。
- [x] 记录全量 Debug 构建与 16 项测试通过的迁移前基线。
- [x] 确认 SHARC 与 NRD 上游仓库、tag 和 commit。

### 1. 核心架构迁移

- [ ] 建立 `render/core`：强类型帧身份、能力集合、Feature 描述、typed blackboard。
- [ ] 建立分域历史策略：TAA、NRD diffuse/specular、SHARC、Atmosphere LUT。
- [ ] 建立 renderer-owned、不可变的 `RenderWorldSnapshot` 和 `SceneDelta`。
- [ ] 将 `ModelRenderer` 与 GI 输入从活动 `Level` 迁移到快照。
- [ ] 将帧协调、Feature Pipeline 和具体 deferred pipeline 的所有权拆开。
- [ ] 为 Camera cut、投影范围、场景灯光和大气参数建立显式 revision。

验证门槛：架构单测、现有 CPU 测试和现有 raster 沙盒全部通过；记录失败帧不得推进历史。

### 2. 构建、Shader ABI 与模块边界

- [ ] 提交兼容 CMake 3.25 的 `CMakePresets.json`，统一 Debug/Release/test 工作流。
- [ ] 将根 CMake 拆为 `src`、`shaders`、`apps/sandbox` 和 `tests` 子目录 target。
- [ ] 建立 `RenderCore`、`VulkanBackend`、`GpuScene`、`RasterLighting`、`Atmosphere`、
  `RayTracing`、`Sharc`、`Nrd`、`HybridGi` 和 `Renderer` 依赖方向。
- [ ] 将 Slang 公共 ABI、entry point 和 feature shader 分目录。
- [ ] 为 shader 编译增加 depfile、warnings-as-errors、反射输出、manifest 和 ABI 校验。
- [ ] 扩展 pipeline factory，支持 compute、ray tracing pipeline 和 shader table。

验证门槛：干净构建目录可用 preset 完成 configure/build/test；修改公共 Slang include 会触发所有依赖重编译。

### 3. Vulkan RT 能力与 GPU Scene

- [x] 实现 `AUTO/ON/OFF` RT 构建开关和运行时 capability tier。
- [ ] 协商并启用 buffer device address、acceleration structure、ray tracing pipeline、ray query 等能力。
- [ ] 将启用的扩展完整传递给 NvRHI Vulkan backend。
- [ ] 建立共享 geometry/material/instance/light buffer 和稳定 `RenderInstanceId`。
- [ ] 根据几何、实例和 transform revision 分别构建/更新 BLAS 与 TLAS。
- [ ] 为无 RT 设备提供明确的 SSAO/raster fallback 和诊断。

验证门槛：capability 单测、无 RT fallback、BLAS/TLAS 更新策略测试和 RT 三角形 smoke test 通过。

### 4. Blinn-Phong 与原始 Ray Traced GI

- [x] 为材质增加明确的 `BlinnPhong` 与 `MetallicRoughness` surface model，并锁定共享 GPU material ABI。
- [x] 实现 Blinn-Phong direct lighting feature，不与 GI backend 耦合。
- [ ] 建立 raygen/miss/closest-hit shader、SBT 和每像素 GI dispatch。
- [ ] 输出独立的 diffuse/specular radiance-hit-distance、viewZ、normal-roughness 和 motion 信号。
- [ ] 由 `LightingCompositeFeature` 最终合成 direct、indirect 与 atmosphere。

验证门槛：raster Blinn-Phong 对照场景、RT miss/hit 场景和信号格式 ABI 测试通过。

### 5. SHARC 集成

- [x] 添加固定 commit 的 `thirdparty/sharc` submodule 与许可证说明。
- [ ] 建立 `SharcRadianceCache` adapter，隔离 vendor shader ABI。
- [ ] 注册 query、resolve、update 和 compact passes，并由 `FrameGraph` 声明所有资源状态。
- [ ] 根据场景/灯光/大气 change set 执行 keep、decay、region invalidate 或 full reset。
- [ ] 提供可视化统计：occupancy、query hit rate、update count 和 overflow。

验证门槛：静态场景收敛、动态实例更新、灯光变化衰减和 cache reset 测试通过。

### 6. NRD 集成

- [x] 添加固定 commit 的 `thirdparty/nrd` submodule 与许可证说明。
- [ ] 将 NRD pipeline/dispatch/resource 描述翻译到 NvRHI，并保持 vendor packing/defines。
- [ ] 明确引擎 motion 与 NRD motion 的方向、单位、jitter 和 world/view normal 转换。
- [ ] 分别管理 diffuse/specular history，并传递 camera cut、resolution change 和 disocclusion。
- [ ] 组合 SHARC/RT noisy signals 与 NRD denoised outputs。

验证门槛：first frame、camera cut、resize、运动物体、禁用/重启 GI 和长时间运行测试通过。

### 7. 大气系统

- [x] 实现 atmosphere 参数、太阳光和 revision 数据模型。
- [x] 审计并记录 `Alpha-Piscium` 的 LUT 拓扑、参数化、更新规则与许可证边界。
- [ ] 实现 transmittance、multi-scattering、sky-view 和 aerial-perspective LUT compute passes。
- [ ] 让 raster sky、Blinn-Phong、RT miss 和 GI 共用同一 atmosphere bindings。
- [ ] 将参数、太阳、视图和分辨率变化映射到最小 LUT 重建集合。

验证门槛：LUT revision 单测、昼夜方向变化、相机高度变化和 raster/RT 天空一致性测试通过。

### 8. 完整验证与文档收口

- [x] 为 CPU、Vulkan、RT、SHARC 和 NRD 测试设置 CTest label/skip/timeout。
- [ ] 运行 Debug/Release 全量构建和测试，启动 sandbox 并检查 Vulkan validation。
- [ ] 捕获 raster、raw SHARC indirect 和 NRD 结果用于回归对比。
- [ ] 完成架构、能力、shader ABI、历史、SHARC indirect、NRD、大气和依赖升级文档。
- [ ] 删除迁移期兼容层、过时源码扫描测试和陈旧 shader 产物依赖。

## 完成定义

只有同时满足以下条件，Goal 才能标记完成：

1. 支持设备上可运行 `RayTracing + SHARC + NRD` GI；不支持设备自动回退且不会创建未启用的 RT 资源。
2. Blinn-Phong、现有 PBR、GI、大气、TAA 和 UI 可通过 Feature 配置组合，不依赖单体 pass recorder。
3. resize、camera cut、场景拓扑/材质/灯光/大气变化均具有测试覆盖的历史失效规则。
4. GI 相关全量测试通过，Vulkan validation 无错误，sandbox 完成实际帧输出验证。
5. 所有新增公共 API 和架构文档使用简体中文说明生命周期与不变量。

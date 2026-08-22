# Lumin 渲染架构

## 应用与游戏边界

依赖方向固定为 `Lumin::Application` -> `Lumin::Render` -> `Lumin::RenderCore` -> `Lumin::Core`。Core 不依赖 Vulkan、
SDL、NvRHI 或渲染器；Render 只读取 Core 的场景和资产接口，Application 是唯一同时组合两者的宿主。
`Lumin::Rendering` 已删除；RenderCore、RenderRhi、VulkanBackend、RenderRuntime、Editor 和各 Feature 名称均对应真实
构建 target。`Application` 的公共头
使用 PImpl，只公开窗口配置、脚本配置和 `Game` 注入点，不泄露 SDL、Vulkan、renderer 或场景的具体所有权。

`Application` 拥有窗口、`Level`、`Camera`、`ScriptRuntime`、主线程 `RenderFramePacketBuilder`、
`RenderSettingsPanelAdapter`、异步 `Renderer` 和 `Editor`。主线程只创建包含 Vulkan instance 与 SDL surface 的
`VulkanSurfaceBootstrap`，随后立即把唯一所有权转入 `Renderer`；专用渲染线程才创建 `VulkanContext`、物理/逻辑设备、
NvRHI、交换链及全部 Feature 资源。退出时先销毁 Feature，再依次销毁交换链、设备、surface 和 instance。
具体游戏通过 `GameContext` 只接收 `Level`、`Camera` 与 `ScriptRuntime`，因此场景初始化和逐帧逻辑可以脱离 Vulkan
测试。`apps/editor` 使用无行为的 `Game` 宿主，不创建演示场景；项目内容只由 `ProjectSession` 创建或加载。

正常退出通过有序控制队列执行 `flush()` 和 `stop()`；异常展开由 `Renderer` 析构等待线程结束。stop 在渲染线程等待
GPU idle，并按 `IRenderPipelineSession -> VulkanContext -> Window` 的顺序释放；窗口与 SDL backend 始终留在主线程。

## 模块化重构状态

新的基础设施已经可独立使用：`RenderFeatureRegistry` 显式注册静态 Feature factory；
`RenderPipelineRecipeResolver` 根据 typed input/output、能力、少量显式依赖和历史域所有权建立 DAG，并拒绝缺失输入、
重复 producer、环和重复历史所有者。`RenderSettingsStore` 按 Feature 类型保存配置，提交时生成不可变快照并把变化归类为
`HotUpdate`、`HistoryReset`、`PipelineRecompose` 或 `ResourceRecreate`。
Editor 只修改独立 `RenderSettingsPanelAdapter` 的聚合面板视图；adapter 在主线程通过 schema 校验并写入 typed store，
Feature 和 Runtime 不包含 ImGui 设置代码。Runtime 始终相对最近成功提交的 settings snapshot 计算变化影响。

`RenderPipelineInstance` 从解析后的执行顺序创建 Feature。Factory 和 `initialize()` 只接收显式
`FeatureCreateContext`；候选初始化失败时逆序 `shutdown()`，旧实例不受影响。一帧内按 DAG 顺序调用 `addPasses()`，
提交成功后正序调用 `onFrameSubmitted()`，失败或主动丢弃时逆序调用 `onFrameDiscarded()`，只有成功提交才推进历史。
resize 和 shutdown 在安全边界按逆序通知，使消费者先释放对上游资源的引用。

帧数据已定义为 `FrameSceneData`、`GpuSceneData`、`RasterSurfaceData`、`RtSurfaceData`、`AtmosphereData`、
`IndirectLightingData`、`DenoisedLightingData`、`SceneHdrData`、`TemporalOutputData` 和 `PresentData`。GPU 数据同时携带
物理 NvRHI handle、本帧唯一 FrameGraph handle、format、extent 和 ready pass。同一物理资源必须经
`FrameGraphResourceImporter` 导入；兼容重复导入复用首个 handle，状态或范围冲突立即失败。

当前运行时使用 `DefaultRenderPipelines` 提供的 Raster/Hybrid recipe，并经
`RenderPipelineRecipeResolver + RenderPipelineInstance` 事务式创建候选管线。Feature 通过显式 factory 注册；旧
`DeferredRenderFeatureSet`、`LevelRenderFeatureKind`、`LevelRenderFeatureHost` 和中央分派 `switch` 已删除。运行时只
消费 `RenderFramePacket`，不保存或读取活动 `Level`、`Camera`、ImGui、SDL 或 Editor。Feature 资源已经拆为
`RasterFeatureResources` 与 `PostFxResources`；旧 `TextureManager` 和
`PipelineManager` 均已删除。各 Feature pipeline handle 由拥有者保存，并统一使用 RenderRhi 的无业务语义
`FullscreenPipelineFactory` 创建。帧内数据已经通过 typed blackboard 隔离，不再存在万能帧数据结构。Hybrid 兼容实现
当前仍复用 Raster surface 的部分物理纹理，后续由 RT surface owner 完全接管。

## 场景更新

`Level` 管理网格、模型实例和 Actor。Actor 句柄包含索引和代数，因此槽位复用后，旧句柄不会解析到新对象。
在 `tick`、`onSpawn` 或 `onDestroy` 中发起的生成与销毁请求会延迟处理，直到当前回调遍历可以安全提交变更。

`Application` 先处理 SDL 事件并调用主线程 `ImGuiFrontend::beginFrame(editor)` 构建当前 ImGui 帧，再读取当前帧
capture 状态。
只有项目已打开且 UI 未捕获输入时才更新相机和派发 `GameInput`，随后调用 `Game::tick`、`Level::tick(deltaSeconds)` 和渲染。
Viewport 图像被悬停并按住鼠标中键时，应用启用 SDL relative mouse mode；该模式隐藏并约束鼠标，以帧内相对位移
更新相机 yaw/pitch，同时继续使用 `WASD`、`Space` 和 `Left Ctrl` 平移。松开右键后立即恢复普通鼠标模式。
因此，即使编辑器捕获输入，游戏模拟、Actor 与 Lua 生命周期仍会推进。主线程随后通过
`RenderFramePacketBuilder` 提取 `RenderWorldSnapshot` 和相机值快照，并在渲染前调用
`ImGuiFrontend::finishFrame()`，将顶点、索引、裁剪命令和 reset-state 命令深拷贝到 `UiDrawPacket`；任意 user callback
会被拒绝。typed settings、窗口/Viewport 物理尺寸和 UI 一并按值进入 `RenderFramePacket`。`drawFrame` 只消费该 packet，
不访问活动场景、相机、ImGui context、SDL backend 或 Editor。场景变化会在 Runtime 中相对最近成功提交的世界快照
重新比较；尚未消费的 packet 即使被替换，也不会漏掉拓扑变化或错误推进历史。
模型变换和材质变化会递增
`modelRevision`；网格或模型成员变化会递增 `topologyRevision`。PBR 纹理路径变化也会递增 `topologyRevision`，
因为材质纹理数组和 descriptor 需要重建；纯标量材质变化只更新对象 buffer。默认 Raster Feature 每帧上传对象记录，
仅在拓扑修订号发生变化时重新构建打包后的几何数据和材质资源。

`Terrain` 生成带索引的高度场网格，通过累积三角形法线得到归一化法线，并支持双线性高度查询。`TerrainActor`
拥有地形数据，将生成的网格附加到 `Level`，并在地形编辑后替换 `Level` 中的网格。

## 帧内顺序

每一帧通过 `FrameGraph` 按以下顺序记录：

1. Raster 路径执行四个 CSM 纯深度通道和 G-buffer；Hybrid 路径由 RT primary surface 直接生成表面信号。
2. 更新当前路径所需的大气 LUT 和全局光照资源。
3. 全局光照 Feature；支持设备执行 GPU Scene、SHARC、RT GI、NRD 与 composite，否则执行 SSAO fallback。
4. 程序化天空盒全屏通道，写入 HDR 光照目标。
5. 延迟光照通道，加载该目标，并结合全局光照输出和 CSM 对几何体进行着色。
6. TAA 解析，读取 HDR 光照、运动矢量和上一帧历史。
7. 将解析结果传输复制到当前历史图像。
8. 使用 ACES 色调映射输出到 renderer 拥有的 Viewport 纹理。
9. ImGui 在独立的 `Viewport` dock window 中采样该纹理，将完整编辑器界面合成到交换链并呈现。

所有图形通道均使用 Vulkan 1.3 动态渲染。`PipelineFactory` 支持 MRT 流水线和仅含顶点阶段的深度流水线；
项目不会创建 `VkRenderPass` 或 framebuffer 对象。

## Renderer Runtime 边界

`Renderer` 是主线程异步门面，公开 `submit(RenderFramePacket)`、`status()`、`flush()` 和 `stop()`。frame mailbox 只保存
一个尚未消费的最新 packet；替换旧 packet 只增加丢帧计数，不推进历史。`flush/stop` 使用独立 FIFO 控制队列，并记录
排队时必须先消费的 frame 序号，因此不会被 latest-wins 替换。最小化窗口的 packet 只被消费，不调用 Vulkan acquire，
也不推进 GPU 历史。启动握手、逐帧异常和确定性退出状态通过 `RendererStatusSnapshot` 发布。

`RenderRuntime` 只依赖 `IRenderPipelineSessionFactory` 与 `IRenderPipelineSession`，不知道默认 recipe、Feature 标识或
任何 Raster/GI/PostFX/Presentation 类型。Application 显式调用 `makeDefaultRenderPipelineSessionFactory()` 注入内置
静态组合；渲染线程在创建 `VulkanContext` 后调用 factory，启动失败会完整回滚并通过握手重新抛到主线程。

`DefaultRenderPipelineSession` 位于 `Lumin::RenderPipelines`，是内置 Raster/Hybrid recipe 的帧事务协调器。它只接收
非拥有 `VulkanContext`、初始 `RenderWorldSnapshotPtr` 和逐帧 `RenderFramePacket`，不保存活动场景、相机、ImGui 或 SDL
引用。`DefaultFeatureRegistry.cpp` 是默认模块唯一显式组合点：Atmosphere、Raster/Hybrid surface、GI、Denoising、
Lighting、TAA、ToneMapping 和 Presentation 都由各自具体 `IRenderFeature` 类型注册并直接处理提交/丢弃生命周期，DAG
resolver 决定执行顺序；没有万能回调 Feature、`LevelRenderFeatureKind`、中央 switch 或字符串式运行时分派。

实现按职责分成以下文件：

- `render/pipelines/default/DefaultRenderPipelineSession.hpp` 保存内置组合的私有所有权和跨帧状态声明。
- `render/pipelines/default/DefaultRenderResources.cpp` 创建、销毁和重建 Viewport、Raster、Atmosphere 与 Hybrid GI 资源。
- `render/pipelines/default/DefaultFeatureRegistry.cpp` 定义并显式注册默认具体 Feature 类型；每个类型直接实现自身的
  `addPasses`、提交和丢弃事务边界。
- `render/core/RenderFramePacket.*` 定义跨线程不可变消息和主线程场景/相机快照 builder。
- `render/runtime/RenderMailbox.*` 定义 latest-wins frame 单槽和不可丢失有序控制队列。
- `render/runtime/Renderer.cpp` 独占渲染线程、启动/退出握手、异常传播与状态发布。
- `render/pipelines/default/DefaultRenderPipelineFrame.cpp` 从 packet 生成渲染提交相关的抖动/阴影数据，导入逐帧资源，创建 framebuffer，
  并执行 `FrameGraph`。
- `render/pipelines/default/DefaultRenderFeatures.cpp` 实现内置模块的 pass setup/record；资源由对应 domain owner 管理。
- `render/core/FrameDataContracts.hpp` 定义跨 Feature 的 typed blackboard 契约；每个 GPU 数据项同时携带物理 NvRHI
  handle、本帧唯一 FrameGraph handle、格式、范围和 ready pass，消费者必须复用 producer 发布的图身份。
- `render/level/FeatureFrameData.hpp` 只保存迁移期实现细节，例如资源导入服务、预创建 framebuffer 和 Hybrid 候选状态；
  其中的非拥有指针、span 和 handle 只在当前 `recordCommandList` 调用期间有效，不得由 Feature 跨帧保存。
- `render/pipelines/DefaultRenderPipelines.*` 定义 Raster/Hybrid recipe。数据 producer/consumer 决定 DAG 主顺序；只有
  Presentation 等外部副作用边界使用少量 `after` 约束。Pipelines 组合层注册各模块 factory，Runtime 不依赖具体模块。

渲染基础设施按物理目录隔离：

- `render/resources/` 包含 `FrameGraph`、`DescriptorIndexingLimits`、`PipelineFactory`、
  `FullscreenPipelineFactory`、`ShaderLibrary` 和 NvRHI 资源包装。Raster/PostFX 业务资源分别位于
  `render/features/raster/` 与 `render/features/postfx/`；通用 factory 不缓存或命名 Feature pipeline；
  `FrameGraph` 只负责外部分配资源的依赖排序与状态转换，
  `DescriptorIndexingLimits` 负责材质纹理 descriptor 的容量预检和绑定计划；两者都不依赖 Editor。
- `render/platform/vulkan/` 包含 `VulkanSurfaceBootstrap`、`VulkanContext` 和 `VulkanRayTracingCapabilities`，是唯一
  允许直接调用原生 Vulkan 设备、交换链和能力查询的目录。bootstrap 在主线程使用 `Window` 创建 instance/surface，
  Context 在渲染线程接管且不保存 `Window&`；交换链
  resize 只消费 packet 中的 `SurfaceState`。单调 `surfaceRevision` 保证携带 resize 事件的 packet 被替换后仍会重建。
- `render/editor/` 包含 `Editor`、`ImGuiContent` 和主线程 `ImGuiFrontend`。该前端独占 ImGui context 与 SDL backend，
  并生成不含外部指针的 `UiDrawPacket` 和 `UiFontAtlas`；`RenderSettingsPanelAdapter` 负责 typed store 适配。
- `render/presentation/` 包含渲染侧 `UiRenderer` 与 `PresentationRenderer`。它们不依赖 ImGui、SDL 或 Editor，只将
  packet 中的稳定 `UiTextureId` 解析为当前 NvRHI 资源并合成到交换链。

`DefaultRenderPipelines` 用 typed inputs/outputs 解析执行顺序。Raster recipe 注册 shadow 与 raster surface；Hybrid
recipe 注册 RT surface/GPU Scene producer，不包含 Raster-only 模块。RT surface descriptor 要求
`AccelerationStructure` 和 `RayTracingPipeline`，缺少时拒绝 Hybrid 候选并由 Runtime 保留旧实例或选择 Raster。
每个时序历史域在一个解析计划中只有一个 Feature 负责（Atmosphere LUT、SHARC、NRD diffuse/specular、TAA），因此历史
失效策略不会散落在渲染器门面中。

一帧的 Feature 事务顺序固定为：`prepareFrame` 按解析后的依赖顺序调用 `addPasses`；录制异常或提交异常调用
`discardFrame`，已进入的 Feature 按逆序收到 `onFrameDiscarded`；队列提交成功后调用 `commitFrame`，按正序发送
`onFrameSubmitted` 并推进各历史域。`presentFrame` 位于 commit 之后，present 失败不会回滚已经提交的 GPU 历史。
因此 ModelRenderer、Atmosphere、GI、NRD、TAA 和 ImGui 的跨帧候选状态都只能在对应 Feature 的提交通知中发布，
丢弃通知必须保持旧版本可重试。

## 表面材质与 GPU ABI

`scene::Material` 位于 `core/scene/Material.hpp`，通过 `SurfaceModel` 逐材质选择
`MetallicRoughness` 或 `BlinnPhong`。枚举使用固定的 `uint32_t` 数值 `0/1`，会直接进入 GPU material buffer；
后续只能追加新值，不能重新排序。材质同时保留两套模型参数，因此编辑器切换模型不会丢失原有调参：
Metallic-Roughness 使用 `roughness/metallic`，Blinn-Phong 使用 `specularColor/shininess`，两者共享 base color、
normal map 和 UV scale。

`gpu::GpuMaterialData` 是 raster、ray tracing、SHARC 与 NRD adapter 共用的 64-byte、16-byte aligned ABI：
`baseColorMetallic` 保存基础颜色与金属度，`specularColorShininess` 保存 Blinn-Phong 高光颜色与指数，
`surfaceParameters` 保存统一等效粗糙度、UV scale 与 normal Y 符号，`metadata` 以整数保存 `SurfaceModel`、
texture descriptor index 和纹理存在标志。Blinn-Phong 指数通过
`sqrt(2 / (shininess + 2))` 转换为 NRD/GI 使用的等效感知粗糙度，避免 direct lighting 与去噪器各自解释材质。

`Material` 可以引用 base color、normal 和 roughness 三张贴图。`ModelRenderer` 对场景中的唯一贴图组合去重，
将其上传为二维数组纹理；第 0 层是白色 base color、平法线和单位粗糙度，未绑定贴图的材质因此沿用相同 shader
与批处理。`GpuMaterialData` 使用 `ModelHandle::index` 稀疏寻址，因此 raster 与 RT 共享同一稳定 material index；
slot 的 generation 被复用时，由逐 frame-slot 的物理 buffer 版本隔离仍在 flight 的旧数据。240-byte `ObjectData`
仅在 `metadata.x` 保存该索引，完整表面模型参数不再塞入 normal/roughness 或其他浮点 G-buffer 通道。

Hybrid primary RT、RT GI 与 SHARC update 复用 `ModelRenderer` 的逐帧 `GpuMaterialData` buffer、base-color/normal-
roughness descriptor arrays 和 repeat sampler，不维护独立材质副本。GPU Scene 的 32-byte `GpuPackedVertex` 将 UV.x/UV.y
分别存入 position/normal 的第四个分量；closest-hit 用重心坐标插值 UV，并以显式 LOD 0 采样 base color 与 roughness。
材质纹理及 buffer 必须使用同一组已导入的 FrameGraph handle 声明 `ShaderResource` 读取，不能只绑定原生 handle 而绕过
资源状态跟踪。

base color 图像使用 `VK_FORMAT_R8G8B8A8_SRGB`，normal RGB 与 roughness 被打包到线性
`VK_FORMAT_R8G8B8A8_UNORM`。G-buffer 将世界空间 normal/roughness 写入一个附件，将线性
albedo/metallic 写入另一个附件，并以独立 `R32_UINT` attachment 输出稳定 material index；无几何像素清为
`GpuMaterialIndex::invalidValue`。direct-lighting 的 descriptor set 0 保留全屏纹理与 uniform，set 1 只绑定
material ID 与 `GpuMaterialData` buffer。`MetallicRoughness` 路径保持 GGX、Smith 与 Schlick BRDF；
`BlinnPhong` 路径读取高光颜色，并从逐像素等效粗糙度反算指数，因此 roughness texture 也会调制其高光宽度。
材质 buffer 的资源状态只在对应 frame slot 成功提交后推进到 `ShaderResource`。OBJ 没有 `vt` 时，加载器生成
带接缝修正的柱面 UV。

## 全局光照后端

`GlobalIlluminationMode::Legacy` 使用 raster G-buffer、CSM、延迟光照与可选的 SSAO、HBAO 或 GTAO；
`GlobalIlluminationMode::RayTracing` 使用 primary/direct RT 和 RT 间接光，不创建或读取 G-buffer/CSM。运行时能力不足、
场景尚无可追踪几何，或构建时使用 `LUMIN_RAY_TRACING=OFF` 时，Ray Tracing 请求会安全回退到 Legacy 拓扑。

Ray Tracing 模式可分别关闭 SHARC 与 NRD。关闭 SHARC 后不录制 cache update/resolve/statistics pass，RT 间接光改用
无辐射缓存的 fallback estimate；关闭 NRD 后，原始 diffuse/specular radiance-hit-distance 直接交给 GI composite。
Legacy 模式可分别关闭屏幕空间 AO 与 CSM。AO 后端共享 position/normal 输入与全分辨率输出：SSAO 使用旋转采样核，
HBAO 对 8 个屏幕方向执行最大地平线搜索，GTAO 对 6 个切片执行双向地平线积分；三者均可调世界空间半径、强度和
几何偏置。CSM 通过 `splitLambda` 和 `maxDistance` 控制四级联分割。TAA 是两条路径共用的
后处理选项。任一模式、Feature 开关或 CSM 参数变化都会使相关时序历史失效；SHARC shader 变体变化还会在等待 GPU
空闲后重建对应 RT GI 资源。

Hybrid GI 的一帧顺序为：按需物化 GPU Scene 和 BLAS/TLAS、执行 SHARC clear/update/resolve、在 RT GI closest-hit
中查询 SHARC、输出 diffuse/specular radiance-hit-distance 与 NRD auxiliary signals、执行 NRD dispatch，最后由
GI composite 写入统一的 RGBA 间接光照目标。RT miss、SHARC update 和 raster sky 使用同一个 atmosphere descriptor
set，避免环境输入在三条路径中漂移。

场景 mesh 在 raster G-buffer 中按双面几何绘制，因此 TLAS instance 固定使用 `TriangleCullDisable`，primary、shadow、
indirect 和 SHARC trace 都不得附加背面剔除 flag。closest-hit 对插值后的世界空间法线执行归一化，并在命中背面时将其
翻向入射光线的反方向；否则绕序不一致的 OBJ、程序化地形或镜像实例会出现缺面，并向直接光、SHARC 和 NRD 传播错误
的半球法线。

`GpuSceneUpdatePlanner::generation()` 表示 GPU 可见内容版本，而不是 CPU 帧号；无 GPU 工作的稳定帧不会推进它。
每个 frame slot 保存一个已提交物理版本：空槽或落后槽在 fence 完成后从最新 immutable snapshot 追赶一次，已经同步的
槽只把现有 buffer 与 TLAS 重新导入 `FrameGraph`，不再创建资源、上传数据或重建 AS。候选版本仅在 queue submit 成功后
发布；录制或提交失败会丢弃候选并保留旧版本，因此其他 in-flight 帧始终引用有效对象。

`VulkanContext::beginFrame()` 在等待当前 frame slot 的 event query 后调用一次
`IDevice::runGarbageCollection()`。NvRHI command buffer 会强引用录制期间使用的 framebuffer、binding set、buffer、
texture 和 AS；该回收是逐帧资源生命周期的一部分，不能仅依赖 C++ handle 离开作用域或最终 device 析构。

新物理版本的 AS 构建固定拆为两个 `FrameGraph` pass：第一个 pass 写入全部活动 BLAS，第二个 pass 将这些 BLAS 声明为
`AccelStructRead` 后再写入 TLAS。两者之间的 `AccelStructWrite -> AccelStructRead` 屏障保证大型 BLAS 的构建结果在 TLAS
读取其设备地址和内容前可见；不得仅依靠命令录制顺序、CPU fence 或捕获工具带来的隐式串行化。

`PostFxResources` 为每个帧槽拥有一张标准 RGBA 全局光照图像。RGB 保存线性间接辐射亮度，alpha 保存环境可见度；
禁用全局光照时的中性值为 `{0, 0, 0, 1}`。屏幕空间 AO 后端写入 `{0, 0, 0, ao}`，延迟光照按
`legacyAmbient * globalIllumination.a + globalIllumination.rgb` 合成环境光。该图像同时支持颜色附件、采样和存储图像
用途，以便后续后端使用光线追踪或计算通道写入相同契约。

相机切换、场景拓扑变化、Legacy/Ray Tracing 模式、AO 算法或参数、Feature 开关变化以及交换链重建都会使后端历史失效。无时序历史的屏幕空间 AO 后端
忽略失效通知；SHARC、NRD diffuse/specular 和 TAA 分域决定 keep、soft reset 或 full reset，并且都只在成功提交后
推进历史。失败帧不会污染下一帧的 previous matrices、jitter、cache 或 denoiser state。

Application 只通过按值 `RendererStatusSnapshot` 向 Editor 提供只读后端信息；切换开关由
`RenderSettingsPanelAdapter` 写入 typed store。该接缝不会让 Rendering 反向依赖 Editor，也不会让游戏代码接触 Vulkan
后端实现。

## Lua 与编辑器工作流

启动时，`Application` 先调用 `Game::initialize`，再通过自身拥有的 `ScriptRuntime` 加载调用方在
`ApplicationConfig::startupScript` 中提供的可选启动脚本。`scriptRoot` 是脚本文件访问边界；加载失败会终止启动并报告
源路径，事务式加载保证失败脚本不留下 Actor。独立 editor 应用不设置启动脚本。

同一个 `ScriptRuntime` 被传给 `Game` 和 `Editor`。编辑器可以查看脚本与诊断、重新加载变更并执行 Lua 控制台命令；
Scene 面板选择仍引用同一个 `Level`。每帧由 Application 构建 immutable packet 并调用 `Renderer::submit()`，因此编辑器
与游戏视图共享设置快照和后端状态，但渲染线程不读取活动对象。ImGui SDL3 后端负责文本输入与 IME，应用层不重复调用
SDL 文本输入 API。

Application 从 SDL 首选目录加载版本化 `engine-settings.json`，并把设置快照及保存回调注入 Editor。没有项目时 Editor
绘制全工作区 `Project Navigator`；有项目时才构建 dockspace。`File > Configuration` 修改启动目标，`View` 和面板标题栏
修改六个面板的全局可见性。缺失或损坏的上次项目不会阻止启动，而是清除该路径并回退到导航器。

## 级联阴影

相机视锥体通过对数与均匀混合方式划分为四段。每一段都使用正交光源投影进行拟合，并对齐到阴影纹素网格以减少抖动。
阴影矩阵使用四个独立的逐帧 uniform buffer，因此记录某一级联时不会覆盖其他级联正在使用的数据。

阴影深度通过显式 `Texture2D.Load` 调用和手动 3x3 PCF 核进行采样，从而无需所选深度格式支持线性过滤。

## 运动矢量与 TAA

场景投影矩阵保持 NDC 的正 Y 朝上，不在矩阵中预先翻转 Y。所有 `nvrhi::Viewport` 使用正的逻辑尺寸；NvRHI 的
Vulkan 后端通过负物理 viewport 高度将正 NDC Y 映射到较小的屏幕 V。因此 NDC 到屏幕 UV 使用
`(0.5 + 0.5 * x, 0.5 - 0.5 * y)`，全屏三角形则使用 `(2 * u - 1, 1 - 2 * v)` 生成裁剪坐标。

`ObjectData` 包含当前及上一帧模型矩阵，以及用于非均匀缩放的逆转置法线矩阵。G-buffer 的帧 uniform 包含当前及
上一帧带抖动的视图投影矩阵。运动附件存储 `currentUv - previousUv`；TAA 以 `currentUv - motion`
重建上一帧的采样位置。

启用 TAA 时，使用以 2 和 3 为底、包含八个样本的 Halton 序列抖动相机投影。每个帧槽写入自己的历史图像并读取
另一个槽位的历史图像，从而在有序图形队列上实现真正的上一帧乒乓缓冲。

以下情况会使历史失效：首次使用、交换链重建、拓扑变化、相机切换、明显的 FOV 变化，以及 TAA 从关闭切换到开启。
第一个有效帧会跳过时序混合。内容有效性与各持久历史图像是否完成初始化分开跟踪；使样本失效不会丢弃其真实的
着色器读取布局和访问状态。因此，该图像被复用时，`FrameGraph` 仍可生成从着色器读取到传输写入所需的依赖。

## 资源所有权

`RasterFeatureResources` 拥有两个帧槽的 G-buffer 与四张 CSM 阴影图；`PostFxResources` 独立拥有标准全局光照输出、
HDR 光照、TAA 解析/历史、后处理 uniform buffer、sampler 和 descriptor set。PostFX 只通过显式
`PostFxBindingInputs` 接收上游 sampled handle，销毁时必须先释放 descriptor set，再释放 Raster producer。
默认 Presentation/PostFX 组合另行拥有一张可作为颜色附件和 sampled image 使用的 Viewport
输出纹理；其物理像素尺寸来自 ImGui Viewport 内容区，尺寸连续两帧稳定后才重建，以免拖动 dock 边界时反复等待 GPU。
尺寸变化通过 `HistoryReason::RenderExtentChanged` 统一失效 TAA、NRD 和 SHARC 等时序状态。
`ModelRenderer` 同样拥有逐帧对象及相机 buffer、四个逐帧阴影矩阵
buffer，以及只读材质数组纹理和采样器。材质纹理在创建时通过 staging buffer 上传并转换到
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`，之后跨帧保持该布局。只有在 `VulkanContext::beginFrame` 等待相应
fence 后，才能更新帧槽。

交换链重建时，系统会等待设备空闲并重新创建 ImGui 的交换链 framebuffer，但保留分辨率由 Viewport 管理的场景资源；
只有 surface format 改变时才重建 Viewport 输出和相关 pipeline。Viewport 尺寸变化走独立的稳定检测与历史失效路径。

## FrameGraph 约定

通道的 setup 回调声明纹理布局、流水线阶段和访问掩码。G-buffer 写入之后，全局光照通道以片段着色器读取位置和
法线，并以颜色附件写入标准输出；延迟光照随后以片段着色器读取该输出。`FrameGraph` 根据这些读写冲突推导顺序，并在每个通道前生成
图像或 buffer barrier。导入的持久纹理还可以提供 `initialStages` 和 `initialAccess`；TAA 历史资源通过它们在不同
提交之间传递同步状态。

`FrameGraph` 当前仅调度和同步外部分配的资源，不负责分配瞬时图像或进行内存别名复用。因此，CSM 仍使用四张
单层图像；若改用数组图像，还需要在 `FrameGraphTextureDesc` 中显式描述层范围。

## NvRHI 与 Vulkan 平台边界

渲染实现基于 NvRHI Vulkan 后端，并通过 NvRHI 使用 Vulkan 1.3 dynamic rendering；渲染器不创建
`VkRenderPass` 或 `VkFramebuffer`。NvRHI 不负责创建交换链。`VulkanSurfaceBootstrap` 与 `VulkanContext` 位于唯一
原生 Vulkan backend 边界：前者只在主线程创建 instance 和 SDL surface，后者在渲染线程接管它们并保留物理/逻辑设备、
队列、交换链与 image view、图像获取/呈现、二进制信号量、能力查询和 NvRHI native interop。交换链图像对应的
NvRHI texture 是非拥有型包装，销毁顺序固定为 renderer 及其子句柄、
交换链 NvRHI 包装、NvRHI device、`VkDevice`。正常销毁和构造中途失败共用幂等清理路径。

`submitFrame` 在同一次图形队列提交中依次调用 `queueWaitForSemaphore`、`queueSignalSemaphore` 和
`executeCommandLists`，随后设置当前帧槽的 `EventQuery`。每个帧槽只复用自己的查询；`beginFrame` 在再次使用该
槽前轮询或等待并执行 `resetEventQuery`。获取/呈现仍由 `VulkanContext` 调用 Vulkan API。

所有逐帧 command list 均调用 `setEnableAutomaticBarriers(false)`。材质纹理与 ImGui 字体使用专用初始化上传列表，
这是仅有的 automatic barrier 例外；上传列表在关闭时把纹理恢复到 `ShaderResource`。只有
`render/resources/FrameGraph.cpp` 可以在运行时代码中调用 `beginTracking*State`、`set*State` 和 `commitBarriers`；首次使用的
纹理以 NvRHI 支持的 `Common`（Vulkan `Undefined` 源布局）导入，离屏附件清理由显式声明 `CopyDest` 的 transfer pass
完成。交换链只保证颜色附件用途：Tonemap 写 Viewport 输出，最终 ImGui pass 将 Viewport 声明为 `ShaderResource`，并将
交换链声明为 `RenderTarget`。后续由 `FrameGraph` 转换到 `RenderTarget` 或 `DepthWrite`；同状态写依赖
使用有效图像布局作为中间状态，绝不把 `Common` 用作 barrier 目标。`Unknown` 只作为“不生成最终转换”的哨兵。
ImGui 每帧顶点和索引数据在对应帧槽 fence 完成后通过 CPU 映射缓冲更新，不进入逐帧上传命令列表。
`historyValid` 表示历史内容能否参与混合，`historyInitialized` 表示持久纹理是否已有
可延续的真实资源状态，两者必须分开维护。

后端策略可按以下命令独立复验：

```powershell
$env:VCPKG_ROOT = "D:/Programs/vcpkg"
cmake -S . -B out/build/nvrhi-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLUMIN_BUILD_TESTS=ON
cmake --build out/build/nvrhi-debug --target lumin_render_backend_policy_tests lumin_render_engine
ctest --test-dir out/build/nvrhi-debug --output-on-failure
.\out\build\nvrhi-debug\LuminEngine.exe
```

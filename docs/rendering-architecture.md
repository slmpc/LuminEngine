# Lumin 渲染架构

## 应用与游戏边界

依赖方向固定为 `Lumin::Core` -> `Lumin::Render` -> `Lumin::Application`。Core 不依赖 Vulkan、SDL、NvRHI 或渲染器；
Render 只读取 Core 的场景和资产接口，Application 是唯一同时组合两者的宿主。旧的 `Runtime`、`Rendering`、`Editor`
和 `GameEngine` 名称仅作为迁移期 alias 保留。`Application` 的公共头
使用 PImpl，只公开窗口配置、脚本配置和 `Game` 注入点，不泄露 SDL、Vulkan、renderer 或场景的具体所有权。

`Application` 拥有窗口、Vulkan 上下文、`Level`、`Camera`、`ScriptRuntime`、`LevelRenderer` 和 `Editor`。
具体游戏通过 `GameContext` 只接收 `Level`、`Camera` 与 `ScriptRuntime`，因此场景初始化和逐帧逻辑可以脱离 Vulkan
测试。沙盒的 OBJ 选择、回退网格、材质和地形全部位于 `apps/sandbox/SandboxGame.*`，不属于通用应用宿主。

renderer 创建后会安装 idle 守卫，正常退出或异常展开都会先调用 `waitIdle()`；成员销毁顺序保证 Editor 和 renderer
先于 Vulkan 上下文与窗口关闭。

## 场景更新

`Level` 管理网格、模型实例和 Actor。Actor 句柄包含索引和代数，因此槽位复用后，旧句柄不会解析到新对象。
在 `tick`、`onSpawn` 或 `onDestroy` 中发起的生成与销毁请求会延迟处理，直到当前回调遍历可以安全提交变更。

`Application` 先处理 SDL 事件并调用 `beginUiFrame(editor)` 构建当前 ImGui 帧，再读取当前帧 capture 状态。
UI 未捕获输入时才更新相机和派发 `GameInput`，随后始终调用 `Game::tick`、`Level::tick(deltaSeconds)` 和渲染。
因此，即使编辑器捕获输入，游戏模拟、Actor 与 Lua 生命周期仍会推进。`drawFrame` 只消费已经准备的 UI 帧；旧调用方
未显式准备时由 renderer 兼容性补建。交换链图像获取提前返回时会取消该 UI 帧，避免重复 `NewFrame` 或遗留活动帧。
模型变换和材质变化会递增
`modelRevision`；网格或模型成员变化会递增 `topologyRevision`。PBR 纹理路径变化也会递增 `topologyRevision`，
因为材质纹理数组和 descriptor 需要重建；纯标量材质变化只更新对象 buffer。`LevelRenderer` 每帧上传对象记录，
仅在拓扑修订号发生变化时重新构建打包后的几何数据和材质资源。

`Terrain` 生成带索引的高度场网格，通过累积三角形法线得到归一化法线，并支持双线性高度查询。`TerrainActor`
拥有地形数据，将生成的网格附加到 `Level`，并在地形编辑后替换 `Level` 中的网格。

## 帧内顺序

每一帧通过 `FrameGraph` 按以下顺序记录：

1. 四个 CSM 纯深度通道，每个级联使用一张独立的二维深度图像。
2. G-buffer 通道：世界空间位置、世界空间法线与粗糙度、线性反照率与金属度、运动矢量和深度。
3. 全局光照 Feature；支持设备执行 GPU Scene、SHARC、RT GI、NRD 与 composite，否则执行 SSAO fallback。
4. 程序化天空盒全屏通道，写入 HDR 光照目标。
5. 延迟光照通道，加载该目标，并结合全局光照输出和 CSM 对几何体进行着色。
6. TAA 解析，读取 HDR 光照、运动矢量和上一帧历史。
7. 将解析结果传输复制到当前历史图像。
8. 使用 ACES 色调映射输出到交换链。
9. 绘制 ImGui 界面并呈现。

所有图形通道均使用 Vulkan 1.3 动态渲染。`PipelineFactory` 支持 MRT 流水线和仅含顶点阶段的深度流水线；
项目不会创建 `VkRenderPass` 或 framebuffer 对象。

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

base color 图像使用 `VK_FORMAT_R8G8B8A8_SRGB`，normal RGB 与 roughness 被打包到线性
`VK_FORMAT_R8G8B8A8_UNORM`。G-buffer 将世界空间 normal/roughness 写入一个附件，将线性
albedo/metallic 写入另一个附件，并以独立 `R32_UINT` attachment 输出稳定 material index；无几何像素清为
`GpuMaterialIndex::invalidValue`。direct-lighting 的 descriptor set 0 保留全屏纹理与 uniform，set 1 只绑定
material ID 与 `GpuMaterialData` buffer。`MetallicRoughness` 路径保持 GGX、Smith 与 Schlick BRDF；
`BlinnPhong` 路径读取高光颜色，并从逐像素等效粗糙度反算指数，因此 roughness texture 也会调制其高光宽度。
材质 buffer 的资源状态只在对应 frame slot 成功提交后推进到 `ShaderResource`。OBJ 没有 `vt` 时，加载器生成
带接缝修正的柱面 UV。

## 全局光照后端

`GlobalIlluminationMode::Auto` 在 Vulkan RT、SHARC 所需 16-bit storage/float16/int64 和 NRD 所需 compute
derivatives 均可用时选择 Hybrid GI，否则使用 `SsaoBackend`。`RayTracedSharcNrd` 表达用户偏好，但运行时能力不足时
仍安全回退；`LUMIN_RAY_TRACING=OFF` 会在构建期同时移除 RT、SHARC、NRD 源码、vendor target 与 shader entries。

Hybrid GI 的一帧顺序为：按需物化 GPU Scene 和 BLAS/TLAS、执行 SHARC clear/update/resolve、在 RT GI closest-hit
中查询 SHARC、输出 diffuse/specular radiance-hit-distance 与 NRD auxiliary signals、执行 NRD dispatch，最后由
GI composite 写入统一的 RGBA 间接光照目标。RT miss、SHARC update 和 raster sky 使用同一个 atmosphere descriptor
set，避免环境输入在三条路径中漂移。

`GpuSceneUpdatePlanner::generation()` 表示 GPU 可见内容版本，而不是 CPU 帧号；无 GPU 工作的稳定帧不会推进它。
每个 frame slot 保存一个已提交物理版本：空槽或落后槽在 fence 完成后从最新 immutable snapshot 追赶一次，已经同步的
槽只把现有 buffer 与 TLAS 重新导入 `FrameGraph`，不再创建资源、上传数据或重建 AS。候选版本仅在 queue submit 成功后
发布；录制或提交失败会丢弃候选并保留旧版本，因此其他 in-flight 帧始终引用有效对象。

`TextureManager` 为每个帧槽拥有一张标准 RGBA 全局光照图像。RGB 保存线性间接辐射亮度，alpha 保存环境可见度；
禁用全局光照时的中性值为 `{0, 0, 0, 1}`。默认 SSAO 后端写入 `{0, 0, 0, ao}`，延迟光照按
`legacyAmbient * globalIllumination.a + globalIllumination.rgb` 合成环境光。该图像同时支持颜色附件、采样和存储图像
用途，以便后续后端使用光线追踪或计算通道写入相同契约。

相机切换、场景拓扑变化、全局光照从关闭切换到开启以及交换链重建都会使后端历史失效。无时序历史的 SSAO 后端
忽略失效通知；SHARC、NRD diffuse/specular 和 TAA 分域决定 keep、soft reset 或 full reset，并且都只在成功提交后
推进历史。失败帧不会污染下一帧的 previous matrices、jitter、cache 或 denoiser state。

GameEngine 只通过 `LevelRenderer::globalIlluminationBackendInfo()` 向 Editor 提供只读后端信息；切换开关写入
`RenderSettings`。该接缝不会让 Rendering 反向依赖 Editor，也不会让游戏代码接触 Vulkan 后端实现。

## Lua 与编辑器工作流

启动时，`Application` 先调用 `Game::initialize` 组装场景，再通过自身拥有的 `ScriptRuntime` 加载可选启动脚本。
`scriptRoot` 是脚本文件访问边界。`--script <path>` 与 `--script=<path>` 都会将显式脚本所在目录设为根目录；默认值为
`apps/sandbox/scripts/sandbox.lua`。加载失败会终止启动并报告源路径，事务式加载保证失败脚本不留下 Actor。

同一个 `ScriptRuntime` 被传给 `Game` 和 `Editor`。编辑器可以查看脚本与诊断、重新加载变更并执行 Lua 控制台命令；
Scene 面板选择仍引用同一个 `Level`。每帧渲染调用 `drawFrame(camera, settings, editor)`，因此编辑器与游戏视图共享渲染
设置和后端状态。ImGui SDL3 后端负责文本输入与 IME，应用层不重复调用 SDL 文本输入 API。

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

`TextureManager` 拥有两个帧槽。每个槽位都有自己的 G-buffer、标准全局光照输出、HDR 光照、TAA 解析/历史、四张阴影图、
后处理 uniform buffer 和 descriptor set。`ModelRenderer` 同样拥有逐帧对象及相机 buffer、四个逐帧阴影矩阵
buffer，以及只读材质数组纹理和采样器。材质纹理在创建时通过 staging buffer 上传并转换到
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`，之后跨帧保持该布局。只有在 `VulkanContext::beginFrame` 等待相应
fence 后，才能更新帧槽。

交换链重建时，系统会等待设备空闲、关闭 ImGui、先于 descriptor layout 和图像销毁流水线、重新创建与尺寸相关的
资源、使时序历史失效，然后再次初始化 ImGui。

## FrameGraph 约定

通道的 setup 回调声明纹理布局、流水线阶段和访问掩码。G-buffer 写入之后，全局光照通道以片段着色器读取位置和
法线，并以颜色附件写入标准输出；延迟光照随后以片段着色器读取该输出。`FrameGraph` 根据这些读写冲突推导顺序，并在每个通道前生成
图像或 buffer barrier。导入的持久纹理还可以提供 `initialStages` 和 `initialAccess`；TAA 历史资源通过它们在不同
提交之间传递同步状态。

`FrameGraph` 当前仅调度和同步外部分配的资源，不负责分配瞬时图像或进行内存别名复用。因此，CSM 仍使用四张
单层图像；若改用数组图像，还需要在 `FrameGraphTextureDesc` 中显式描述层范围。

## NvRHI 与 Vulkan 平台边界

渲染实现基于 NvRHI Vulkan 后端，并通过 NvRHI 使用 Vulkan 1.3 dynamic rendering；渲染器不创建
`VkRenderPass` 或 `VkFramebuffer`。NvRHI 不负责创建交换链。`VulkanContext` 是唯一原生 Vulkan 边界，保留
实例、物理/逻辑设备、队列、SDL surface、交换链与 image view、图像获取/呈现、二进制信号量、能力查询和
NvRHI native interop。交换链图像对应的 NvRHI texture 是非拥有型包装，销毁顺序固定为 renderer 及其子句柄、
交换链 NvRHI 包装、NvRHI device、`VkDevice`。正常销毁和构造中途失败共用幂等清理路径。

`submitFrame` 在同一次图形队列提交中依次调用 `queueWaitForSemaphore`、`queueSignalSemaphore` 和
`executeCommandLists`，随后设置当前帧槽的 `EventQuery`。每个帧槽只复用自己的查询；`beginFrame` 在再次使用该
槽前轮询或等待并执行 `resetEventQuery`。获取/呈现仍由 `VulkanContext` 调用 Vulkan API。

所有逐帧 command list 均调用 `setEnableAutomaticBarriers(false)`。材质纹理与 ImGui 字体使用专用初始化上传列表，
这是仅有的 automatic barrier 例外；上传列表在关闭时把纹理恢复到 `ShaderResource`。只有
`render/FrameGraph.cpp` 可以在运行时代码中调用 `beginTracking*State`、`set*State` 和 `commitBarriers`；首次使用的
纹理以 NvRHI 支持的 `Common`（Vulkan `Undefined` 源布局）导入，离屏附件清理由显式声明 `CopyDest` 的 transfer pass
完成，交换链则由全屏 Tonemap pass 直接覆盖。后续由 `FrameGraph` 转换到 `RenderTarget` 或 `DepthWrite`；同状态写依赖
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

# Lumin 渲染架构

## 场景更新

`Level` 管理网格、模型实例和 Actor。Actor 句柄包含索引和代数，因此槽位复用后，旧句柄不会解析到新对象。
在 `tick`、`onSpawn` 或 `onDestroy` 中发起的生成与销毁请求会延迟处理，直到当前回调遍历可以安全提交变更。

`Application` 更新相机输入，调用 `Level::tick(deltaSeconds)`，然后执行渲染。模型变换和材质变化会递增
`modelRevision`；网格或模型成员变化会递增 `topologyRevision`。PBR 纹理路径变化也会递增 `topologyRevision`，
因为材质纹理数组和 descriptor 需要重建；纯标量材质变化只更新对象 buffer。`LevelRenderer` 每帧上传对象记录，
仅在拓扑修订号发生变化时重新构建打包后的几何数据和材质资源。

`Terrain` 生成带索引的高度场网格，通过累积三角形法线得到归一化法线，并支持双线性高度查询。`TerrainActor`
拥有地形数据，将生成的网格附加到 `Level`，并在地形编辑后替换 `Level` 中的网格。

## 帧内顺序

每一帧通过 `FrameGraph` 按以下顺序记录：

1. 四个 CSM 纯深度通道，每个级联使用一张独立的二维深度图像。
2. G-buffer 通道：世界空间位置、世界空间法线与粗糙度、线性反照率与金属度、运动矢量和深度。
3. SSAO 全屏通道，读取世界空间位置和法线。
4. 程序化天空盒全屏通道，写入 HDR 光照目标。
5. 延迟光照通道，加载该目标，并结合 SSAO 和 CSM 对几何体进行着色。
6. TAA 解析，读取 HDR 光照、运动矢量和上一帧历史。
7. 将解析结果传输复制到当前历史图像。
8. 使用 ACES 色调映射输出到交换链。
9. 绘制 ImGui 界面并呈现。

所有图形通道均使用 Vulkan 1.3 动态渲染。`PipelineFactory` 支持 MRT 流水线和仅含顶点阶段的深度流水线；
项目不会创建 `VkRenderPass` 或 framebuffer 对象。

## PBR 材质

`Material` 可以引用 base color、normal 和 roughness 三张贴图。`ModelRenderer` 对场景中的唯一贴图组合去重，
将其上传为二维数组纹理；第 0 层是白色 base color、平法线和单位粗糙度，未绑定贴图的材质因此沿用相同 shader
与批处理。对象记录保存数组层、UV 缩放、粗糙度/金属度因子和 normal Y 符号，MDI 绘制无需逐模型切换 descriptor。

base color 图像使用 `VK_FORMAT_R8G8B8A8_SRGB`，normal RGB 与 roughness 被打包到线性
`VK_FORMAT_R8G8B8A8_UNORM`。G-buffer 将世界空间 normal/roughness 写入一个附件，将线性
albedo/metallic 写入另一个附件。延迟光照使用 GGX 法线分布、Smith 几何遮蔽和 Schlick Fresnel；材质缺少
metallic/AO 贴图时分别使用介电常量和 SSAO。OBJ 没有 `vt` 时，加载器生成带接缝修正的柱面 UV。

## 级联阴影

相机视锥体通过对数与均匀混合方式划分为四段。每一段都使用正交光源投影进行拟合，并对齐到阴影纹素网格以减少抖动。
阴影矩阵使用四个独立的逐帧 uniform buffer，因此记录某一级联时不会覆盖其他级联正在使用的数据。

阴影深度通过显式 `Texture2D.Load` 调用和手动 3x3 PCF 核进行采样，从而无需所选深度格式支持线性过滤。

## 运动矢量与 TAA

`ObjectData` 包含当前及上一帧模型矩阵，以及用于非均匀缩放的逆转置法线矩阵。G-buffer 的帧 uniform 包含当前及
上一帧带抖动的视图投影矩阵。运动附件存储 `currentUv - previousUv`；TAA 以 `currentUv - motion`
重建上一帧的采样位置。

启用 TAA 时，使用以 2 和 3 为底、包含八个样本的 Halton 序列抖动相机投影。每个帧槽写入自己的历史图像并读取
另一个槽位的历史图像，从而在有序图形队列上实现真正的上一帧乒乓缓冲。

以下情况会使历史失效：首次使用、交换链重建、拓扑变化、相机切换、明显的 FOV 变化，以及 TAA 从关闭切换到开启。
第一个有效帧会跳过时序混合。内容有效性与各持久历史图像是否完成初始化分开跟踪；使样本失效不会丢弃其真实的
着色器读取布局和访问状态。因此，该图像被复用时，`FrameGraph` 仍可生成从着色器读取到传输写入所需的依赖。

## 资源所有权

`TextureManager` 拥有两个帧槽。每个槽位都有自己的 G-buffer、SSAO、HDR 光照、TAA 解析/历史、四张阴影图、
后处理 uniform buffer 和 descriptor set。`ModelRenderer` 同样拥有逐帧对象及相机 buffer、四个逐帧阴影矩阵
buffer，以及只读材质数组纹理和采样器。材质纹理在创建时通过 staging buffer 上传并转换到
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`，之后跨帧保持该布局。只有在 `VulkanContext::beginFrame` 等待相应
fence 后，才能更新帧槽。

交换链重建时，系统会等待设备空闲、关闭 ImGui、先于 descriptor layout 和图像销毁流水线、重新创建与尺寸相关的
资源、使时序历史失效，然后再次初始化 ImGui。

## FrameGraph 约定

通道的 setup 回调声明纹理布局、流水线阶段和访问掩码。`FrameGraph` 根据读写冲突推导顺序，并在每个通道前生成
图像或 buffer barrier。导入的持久纹理还可以提供 `initialStages` 和 `initialAccess`；TAA 历史资源通过它们在不同
提交之间传递同步状态。

`FrameGraph` 当前仅调度和同步外部分配的资源，不负责分配瞬时图像或进行内存别名复用。因此，CSM 仍使用四张
单层图像；若改用数组图像，还需要在 `FrameGraphTextureDesc` 中显式描述层范围。

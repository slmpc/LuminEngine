# Lumin Engine

Lumin Engine 是一个使用 C++20、SDL3、Slang 和动态渲染构建的紧凑型 Vulkan 1.3 渲染器与场景沙盒。

当前沙盒包含：

- 由 `Level` 管理、支持延迟生成与销毁及逐帧 `Tick` 的 Actor 系统。
- 可生成法线并支持高度查询的程序化高度场地形。
- 包含位置、法线与粗糙度、反照率与金属度、运动矢量的 G-buffer 延迟渲染器。
- 支持 sRGB base color、切线空间 normal 和 roughness 贴图，以及 GGX/Cook-Torrance PBR 光照。
- 四级联方向光阴影、可替换的全局光照后端（默认使用 SSAO）和程序化天空盒。
- 使用 Halton 抖动，并结合上一帧相机与模型运动矢量的 TAA。
- ACES 色调映射，以及用于编辑场景、渲染设置和 Lua 脚本的原生停靠式编辑器。

## 目录结构

- `apps/sandbox`：可运行的示例程序。
- `core`：不依赖图形 API 的场景、资产、脚本和 Game API。
- `render`：独立渲染模块，包含 Vulkan/NvRHI、FrameGraph、GPU Scene、GI、Atmosphere 和编辑器 UI。
- `application`：连接 Core 与 Render 的应用组合层；游戏逻辑不会反向进入 Render。
- `assets/models`：实验时放置 OBJ 文件的目录。
- `assets/materials`：本地 PBR 材质贴图目录，由下载脚本生成且不纳入版本控制。
- `scripts`：资源下载等项目辅助脚本。
- `shaders`：着色器源码。
- `cmake`：仅保留依赖策略、目标公共设置和 shader 工具发现等少量 CMake 辅助模块。
- `docs`：简要架构说明。

模块边界、target alias、CMake 入口和 shader companion JSON 约定见
[`docs/module-architecture.md`](docs/module-architecture.md)。

## 依赖项

项目通过 `vcpkg.json` 声明以下依赖：

- SDL3：窗口创建、事件处理和 Vulkan surface 集成。
- GLM：数学类型。
- Dear ImGui：渲染器界面。
- tinyobjloader：OBJ 文件解析。
- stb_image：JPEG/PNG 材质贴图解码。
- Vulkan headers 和 loader。

如果 vcpkg 不在 `CMakeLists.txt` 检查的常用路径中，请设置 `VCPKG_ROOT`。Vulkan SDK 必须提供
`slangc`，以便 CMake 将 `shaders` 下的文件编译为 SPIR-V。

## 构建

```powershell
cmake --preset debug
cmake --build out/build/debug
```

Vulkan Ray Tracing 后端由三态 cache 变量控制：

```powershell
cmake --preset debug -DLUMIN_RAY_TRACING=AUTO
cmake --preset debug -DLUMIN_RAY_TRACING=ON
cmake --preset debug -DLUMIN_RAY_TRACING=OFF
```

- `AUTO`（默认）：编入 RT 实现；运行时在完整 RT 设备上启用，否则回退到 raster。
- `ON`：编入 RT 实现；仍由运行时策略决定是否强制要求设备能力。
- `OFF`：不提供 RT 实现，也不配置 SHARC/NRD vendor target；运行时不会查询或启用 RT 专属 Feature 与扩展。

其他值会在 configure 阶段报错。该选择会写入生成头
`generated/include/render/RayTracingBuildConfiguration.hpp`，属于构建产物能力，不能由运行时描述覆盖。

运行沙盒：

```powershell
.\out\build\debug\LuminEngine.exe
```

沙盒默认加载仓库内的 `apps/sandbox/scripts/sandbox.lua`。也可以用以下两种等价形式指定启动脚本：

```powershell
.\out\build\debug\LuminEngine.exe --script .\my-game\startup.lua
.\out\build\debug\LuminEngine.exe --script=.\my-game\startup.lua
```

显式脚本路径的父目录会成为该次运行的 `scriptRoot`。脚本及其派生加载只能访问此目录内的文件；缺失、语法错误或
越界路径会在启动时返回明确错误，且不会留下无主的脚本 Actor。

也可以使用 OBJ 文件替换默认模型：

```powershell
.\out\build\debug\LuminEngine.exe .\assets\models\your_model.obj
```

如需在 SDL 和 Vulkan 初始化前挂载 RenderDoc，请同时传入启用参数和 RenderDoc 动态库路径：

```powershell
.\out\build\debug\LuminEngine.exe --renderdoc=true --renderdoc-path "C:\Program Files\RenderDoc\renderdoc.dll"
```

`--renderdoc` 是 `--renderdoc=true` 的简写；显式值支持 `true`、`false`、`1`、`0`、`on` 和 `off`。
仓库中的 VS Code CMake 运行/调试配置会使用 RenderDoc 的 Windows 标准安装路径启用它。如果 RenderDoc 安装在其他位置，
请修改 `.vscode/settings.json` 中的 `cmake.debugConfig.args`；如需禁用，则传入 `--renderdoc=false`。

Debug 配置过程还会生成 `out/build/debug/compile_commands.json`。本地 VS Code 设置会自动将该目录传给 clangd。
修改 CMake 文件或依赖项后，可使用以下命令刷新：

```powershell
cmake -S . -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

使用 `WASD`、`Space` 和 `Left Ctrl` 移动相机。编辑器面板可选择 Actor、模型或脚本，修改变换与材质，调整相机、
CSM、全局光照、TAA、曝光和太阳方向，并在 Lua 控制台执行表达式（例如 `return 6 * 7`）。当编辑器正在接收键盘、
鼠标或文本输入时，应用会抑制 `Escape` 和相机控制，但不会暂停 `Game::tick`、`Level::tick` 或 Lua 生命周期。

如果未提供 OBJ 路径，沙盒会优先加载 `assets/models/stanford-bunny.obj`；该文件不可用时则使用内置立方体。
场景还会创建一个程序化地形 Actor 和第二个内置网格。

默认主模型使用 `assets/materials/aerial_asphalt_01` 下的沥青材质。base color 按 sRGB 解码，OpenGL normal
贴图在采样时修正 Y 方向，roughness 写入 G-buffer 的法线附件 alpha，metallic 写入反照率附件 alpha。
该材质没有 metallic 或 AO 贴图，因此使用 `metallic=0`，环境遮蔽由默认 SSAO 全局光照后端提供。缺少 `vt` 的 OBJ 会在加载时
生成柱面 UV。当前材质路径不执行几何位移或视差映射。

材质贴图不纳入版本控制。首次运行沙盒前，请使用 Python 3 下载清单中的全部材质：

```powershell
python scripts/download_materials.py
```

脚本会校验每个文件的 SHA-256，并跳过已经完整下载的文件。可使用
`python scripts/download_materials.py --check` 进行离线完整性检查，或使用 `--force` 重新下载全部文件。
当前清单中的 Aerial Asphalt 01 来自 [Poly Haven](https://polyhaven.com/a/aerial_asphalt_01)，采用 CC0 许可。

## 测试

```powershell
ctest --test-dir out\build\debug --output-on-failure
```

测试覆盖相机移动、`Level`/模型修订号、Actor 生命周期与延迟变更、地形生成与高度采样、PBR 图像解码、
缺失 UV 生成、渲染器材质批次构建、全局光照后端契约、Vulkan/RT capability 与 fallback、无 Vulkan 的
`Game` 生命周期、启动脚本错误隔离和输入路由。可使用 `cpu`、`vulkan`、`raytracing`、`sharc`、`nrd`、
`hardware` 和 `fallback` 标签选择测试；真实硬件测试在缺少前置能力时以 CTest skip 返回。

有关标签、硬件跳过与发布门槛，请参阅 `docs/testing.md`；渲染通道顺序、时序历史约定和资源所有权模型见
`docs/rendering-architecture.md`。

## NvRHI 渲染后端边界

渲染器使用 NvRHI 的 Vulkan 后端和 Vulkan 1.3 动态渲染。NvRHI 不创建交换链：`VulkanContext` 仍负责 SDL
surface、Vulkan 实例与设备、交换链及 image view、图像获取和呈现，并把交换链图像包装为非拥有型 NvRHI
texture。帧提交在同一次图形队列提交中严格按 `queueWaitForSemaphore`、`queueSignalSemaphore`、
`executeCommandLists` 排列；每个帧槽复用独立的 `EventQuery`，再次写入该槽前必须等待并重置查询。

所有逐帧命令列表都关闭 NvRHI automatic barriers；只有材质纹理与 ImGui 字体的专用初始化上传列表启用它，以上传并
恢复到 `ShaderResource`。生产代码中只有 `FrameGraph` 可以在运行时调用显式 resource-state tracking 和 barrier API；
附件清理由独立的 `CopyDest` transfer pass 声明。跨提交资源通过导入状态延续真实状态。TAA 分别记录 `historyValid` 与
`historyInitialized`：内容失效不会抹去已初始化纹理的真实布局/访问状态。

可使用独立目录配置、构建并验证后端策略：

```powershell
$env:VCPKG_ROOT = "D:/Programs/vcpkg"
cmake -S . -B out/build/nvrhi-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLUMIN_BUILD_TESTS=ON
cmake --build out/build/nvrhi-debug --target lumin_render_backend_policy_tests lumin_render_engine
ctest --test-dir out/build/nvrhi-debug --output-on-failure
.\out\build\nvrhi-debug\LuminEngine.exe
```

# 模块与构建架构

项目由两个大模块和一个很薄的组合层组成：

```text
                 Application
                /             \
               v               v
             Core             Render
                              |
                              v
                           Core API

  Core: Game / Scene / Assets / Scripting
  Render: platform / resources / features / editor
```

箭头表示“依赖”。`Application` 是组合层，同时依赖 Core 和 Render；Render 只依赖 Core 的数据接口，Core 不依赖
图形 API。Editor 应用位于 Application 之上，使用无行为的 `Game` 宿主启动项目工作流。

## Core

`core/` 只包含 CPU 侧可复用能力：`scene`、`assets`、`project`、`scripting` 和 `game`。它的 public include
路径保持为 `scene/...`、`assets/...`、`scripting/...` 和 `game/...`，但物理文件已经归入 Core 目录。
Core 不链接 Vulkan、SDL、NvRHI、Dear ImGui 或任何 renderer target，因此游戏逻辑和 CPU 测试可以在没有
图形设备的环境中编译和运行。

`project` 保存版本化项目/场景文件、稳定 `AssetId`、资源注册表和导入事务。它只操作 `Level`、`Camera` 与
`ScriptRuntime`；SDL 文件对话框和 Dear ImGui 窗口仍属于 Application/Render。

## Render

`render/` 是独立的 GPU 模块，统一拥有窗口平台适配、Vulkan/NvRHI backend、资源状态跟踪、GPU Scene、
deferred/Blinn-Phong、RT DI/GI、SHARC、NRD、大气 LUT、ImGui renderer 和 Editor UI。它只通过 `scene`、
`assets` 等 Core API 消费场景数据，不包含 `Application` 或 `Game` 的生命周期控制。物理目录按依赖边界划分，
命名空间保持为 `lumin::render`，因此迁移不会改变外部 API：

- `render/platform/` 保存窗口和调试适配；`render/platform/vulkan/` 只保存原生 Vulkan 上下文、交换链、
  能力探测以及与 NvRHI 的 native interop。
- `render/resources/` 保存 `FrameGraph`、`DescriptorIndexingLimits`、NvRHI buffer/texture 包装、
  `TextureManager`、pipeline 创建/缓存和 `ShaderLibrary`。这些类型负责资源描述、状态跟踪、绑定计划和资源生命周期，
  不包含编辑器窗口逻辑。
- `render/editor/` 保存 `Editor`、`ImGuiContent`、`ImGuiLayer` 和 `ImGuiManager`。该目录负责 UI 帧、
  ImGui 输入捕获和交换链绘制，渲染资源通过 `resources` 的公共 API 注入。
- `render/level/`、`render/features/`、`render/gi/`、`render/atmosphere/` 和 `render/gpu/` 保存场景渲染功能；
  `render/gi/legacy/` 保存 raster fallback，`render/gi/raytracing/` 保存 RTDI、RTGI、SHARC、NRD 与 composite，
  通过 `resources` 声明和使用资源，不直接操作原生 Vulkan 状态。

`Lumin::Render` 是唯一真实的渲染静态库。`Lumin::Rendering`、`Lumin::RenderCore`、`Lumin::Editor`、
`Lumin::VulkanBackend` 和 `Lumin::RenderFeatures` 仅是迁移期 alias，避免外部测试一次性修改所有链接名。

## Application

`application/Application.cpp` 是组合层，不属于 Core 或 Render 的内部实现。它创建窗口、Vulkan 上下文、
场景、脚本和 `LevelRenderer`，并把 `Game` 注入到 Core 的 `GameContext`。`Lumin::Application`（兼容名
`Lumin::GameEngine`）链接 `Lumin::Core` 与 `Lumin::Render`；Render 不反向链接 Application。

## CMake 入口

顶层 `CMakeLists.txt` 只负责工具链、依赖和组合层。大模块各自拥有一个构建文件：

- `core/CMakeLists.txt`：Core 的全部源文件和依赖；
- `render/CMakeLists.txt`：Render 的全部源文件、可选 NRD/SHARC 源码和 alias；
- `shaders/CMakeLists.txt`：调用 Python shader generator 并注册 shader targets；
- `apps/editor/CMakeLists.txt`、`tests/CMakeLists.txt`：应用与验证目标。

不再为 atmosphere、GI、GPU Scene 等内部子系统创建独立的 CMake 文件。新增源文件只需要加入所属大模块
的一个列表；新增 shader 只需要添加 companion JSON，不需要编辑 CMake JSON 解析逻辑。

## Shader 配置

每个 shader 入口 `.slang` 都有同名 `.json`，例如 `RtDi.slang`/`RtDi.json`。shader 源文件、companion JSON 与
生成产物的 stem 统一使用大驼峰。JSON 记录该源文件的入口、stage、输出、
reflection、depfile、capability、binding 和 `requires` feature。公共 ABI 与默认 compiler 选项放在
`shaders/shader-abi.json`。

配置阶段由 `scripts/shader_manifest.py generate` 扫描并校验所有 companion JSON，然后在 build tree 写入
临时 `shader-manifest.json` 和 `shader-targets.cmake`。CMake 不再解析 JSON，也不再维护入口列表；它只
include 生成文件并定义 `lumin_shader_abi` / `lumin_shaders` 两个 target。`LUMIN_RAY_TRACING`、
`LUMIN_ENABLE_NRD` 和 `LUMIN_ENABLE_SHARC` 会在生成阶段过滤对应 entry。

`.slang` 源码和其 include 依赖由 `slangc` depfile 在构建阶段跟踪，保存源码不会触发 CMake 重新配置；
companion JSON、`shader-abi.json` 或生成脚本变化时才重新生成构建描述。生成内容未变化时会保留文件时间戳，
并且每个编译命令只依赖自身源码，因此普通配置刷新和单个 shader 修改都不会导致全量 shader 重编译；
shader ABI 校验同样只在 manifest、reflection 或校验器发生变化后运行。

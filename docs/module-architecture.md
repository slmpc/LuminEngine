# 模块与构建架构

项目由 Core、分层 Render 静态库和一个很薄的 Application 组合层组成：

```text
                         Application
                       /      |      \
                      v       v       v
                   Editor   Render   Core
                              |
             +----------------+----------------+
             v                v                v
       RenderRuntime    RenderFeatures   RenderPipelines
             |                |                |
             +--------+-------+----------------+
                      v
        VulkanBackend / RenderRhi / RenderCore
                                      |
                                      v
                                    Core
```

箭头表示“依赖”。`Application` 是组合层，同时依赖 Core、Render 门面和 Editor；任何 Render target 都不得反向依赖
Application。Editor 应用位于 Application 之上，使用无行为的 `Game` 宿主启动项目工作流。

## Core

`core/` 只包含 CPU 侧可复用能力：`scene`、`assets`、`project`、`scripting` 和 `game`。它的 public include
路径保持为 `scene/...`、`assets/...`、`scripting/...` 和 `game/...`，但物理文件已经归入 Core 目录。
Core 不链接 Vulkan、SDL、NvRHI、Dear ImGui 或任何 renderer target，因此游戏逻辑和 CPU 测试可以在没有
图形设备的环境中编译和运行。

`project` 保存版本化项目/场景文件、稳定 `AssetId`、资源注册表和导入事务。它只操作 `Level`、`Camera` 与
`ScriptRuntime`；SDL 文件对话框和 Dear ImGui 窗口仍属于 Application/Render。

## Render

`render/` 由多个真实静态库组成，分别拥有 Core 契约、NvRHI 资源、Vulkan backend、Feature 域、Runtime 和 Editor。
这些 target 不是指向同一个单体库的 alias。`Lumin::Render` 只是组合内置 Runtime、Feature 和 recipe 的 INTERFACE 门面，
自身不编译源码。

当前 target 及职责如下：

- `Lumin::RenderCore`：帧身份、`RenderFramePacket`、主线程快照 builder、历史策略、typed blackboard、typed frame-data
  contract、Feature registry、recipe DAG、typed settings 和 `RenderPipelineInstance` 生命周期事务；不依赖 SDL、
  原生 Vulkan 或 Editor。
- `Lumin::RenderRhi`：`FrameGraph`、资源导入去重、`ShaderLibrary`、无业务语义的 Pipeline/资源 factory；只面向 NvRHI。
- `Lumin::VulkanBackend`：窗口 surface、`VulkanContext`、交换链、能力探测和 RenderDoc 接入；这是唯一原生 Vulkan 边界。
- `Lumin::Atmosphere`、`Lumin::GpuScene`、`Lumin::RasterFeatures`、`Lumin::GiFeatures`、`Lumin::PostFxFeatures`：按资源
  所有权域拆分的 Feature 实现。
- `Lumin::RenderPresentation`：交换链 framebuffer 与 UI NvRHI renderer；在渲染主线程同步消费当前帧
  `ImDrawData`，不保存 ImGui context、SDL backend 或 Editor 对象。
- `Lumin::RenderPipelines`：Raster/Hybrid recipe、typed producer/consumer 契约、默认模块显式注册和
  `DefaultRenderPipelineSession` 帧事务组合。
- `Lumin::RenderRuntime`：同步 `Renderer` 门面、状态快照和抽象 `IRenderPipelineSession` 生命周期；不链接或包含
  任何具体 Feature/recipe。Renderer 的构造、`drawFrame()` 和销毁都在拥有 SDL 窗口的渲染主线程执行。
- `Lumin::Editor`：独立 Editor UI target，不再与 Render 单体库共享产物。

物理目录继续按依赖边界划分：

- `render/platform/` 保存窗口和调试适配；`render/platform/vulkan/` 只保存原生 Vulkan 上下文、交换链、
  能力探测以及与 NvRHI 的 native interop。
- `render/core/` 保存后端无关的规划、设置和帧事务契约；`render/resources/` 保存 NvRHI FrameGraph 和通用 factory。
  旧 `TextureManager` 已拆为 `render/features/raster/RasterFeatureResources` 与
  `render/features/postfx/PostFxResources`；旧 `PipelineManager` 已由无业务语义的 `FullscreenPipelineFactory` 和各
  Feature 自有 handle 替代。
- `render/editor/` 保存 `Editor`、`EditorLogicSnapshot`、`ImGuiContent` 和 `ImGuiFrontend`。该目录在渲染主线程
  独占 ImGui context 与 SDL backend，`finishFrame()` 返回仅供当前帧同步消费的 `ImDrawData`；
  `RenderSettingsPanelAdapter` 把面板聚合值写入 typed store。依赖关系不能反向流入 Runtime 或 Feature。
- `render/presentation/` 保存 `UiRenderer` 与 `PresentationRenderer`，以稳定逻辑纹理 ID 解析字体和 Viewport 物理资源，
  不保存 ImGui command、callback 或 NvRHI binding 指针形式的纹理 ID。
- `render/level/`、`render/features/`、`render/gi/`、`render/atmosphere/` 和 `render/gpu/` 保存场景渲染功能；
  `render/gi/legacy/` 保存 raster fallback，`render/gi/raytracing/` 保存 RTDI、SHARC update/indirect、NRD 与 composite，
  通过 `resources` 声明和使用资源，不直接操作原生 Vulkan 状态。

`Lumin::Rendering` 已删除。`Lumin::RenderCore`、`Lumin::RenderRhi`、`Lumin::VulkanBackend`、各 Feature target、
`Lumin::RenderRuntime` 和 `Lumin::Editor` 都对应独立构建产物；只有 `Lumin::Render` 与 `Lumin::RenderFeatures` 是不隐藏
源码的组合 target。

## Application

`application/Application.cpp` 是组合层，不属于 Core 或 Render 的内部实现。它启动 `LogicRuntime`，后者在独立逻辑线程
独占 `Game`、`Level`、Camera 镜像、`ScriptRuntime` 与 `ProjectSession`，并向渲染主线程发布不可变
`EditorLogicSnapshot`。Editor 修改通过有序命令队列返回逻辑线程，Game 按键采用 latest-wins 状态。Camera 镜像也采用
latest-wins，但发布不会唤醒逻辑线程，并在下一次固定 Tick 或 Editor 命令前同步，供 `GameContext` 与项目保存使用。

渲染主线程创建并持有窗口、SDL backend、ImGui context、`RenderFramePacketBuilder`、settings adapter、`Renderer`、
`VulkanContext`、device、NvRHI、swapchain 与权威 Viewport Camera。Camera 依据 SDL 输入和真实渲染 delta 逐帧更新，
不受项目 `logicTickHz` 限制；Renderer、Picking、ImGuizmo、拖放和 Camera 面板共享该实例。每个 UI 帧只读取一次逻辑
快照，`RenderFramePacketBuilder` 组合其中的世界快照与渲染线程 Camera；`Renderer::drawFrame()` 在当前线程立即记录、
提交并呈现，不读取活动逻辑对象。
`Lumin::Application`（兼容名
`Lumin::GameEngine`）链接 `Lumin::Core` 与 `Lumin::Render`；Render 不反向链接 Application。

## CMake 入口

顶层 `CMakeLists.txt` 只负责工具链、依赖和组合层。大模块各自拥有一个构建文件：

- `core/CMakeLists.txt`：Core 的全部源文件和依赖；
- `render/CMakeLists.txt`：RenderCore、RHI、Vulkan backend、Feature 域、Runtime、Pipelines、Presentation 和 Editor；
- `shaders/CMakeLists.txt`：编译并调用 C++ Shader Catalog generator，注册 shader targets；
- `apps/editor/CMakeLists.txt`、`tests/CMakeLists.txt`：应用与验证目标。

Feature target 必须能够独立编译和测试。新增源码应加入实际拥有它的 target，禁止为绕过链接错误将源码重复放入多个
静态库，也禁止重新建立多个名称指向同一库的伪模块。新增 shader 入口应扩展 `ShaderId` 并在集中 Catalog 中配置，
不需要编辑 CMake 或手写 JSON。

## Shader 配置

所有入口由 `render/resources/ShaderCatalog.cpp` 的 `ShaderCatalogBuilder` 集中声明。`ShaderId` 提供稳定运行时身份，
module builder 复用源文件、feature、capability、binding 和 ABI 结构配置，并为每个入口生成 SPIR-V、reflection 与
depfile 路径。

配置阶段会用标准 C++23 编译并运行 `ShaderCatalogGenerator`，在 build tree 写入临时 `shader-manifest.json` 和
`shader-targets.cmake`。CMake 不解析 JSON，也不维护入口列表；它只 include 生成文件并定义
`lumin_shader_abi` / `lumin_shaders` 两个 target。`LUMIN_RAY_TRACING`、
`LUMIN_ENABLE_NRD` 和 `LUMIN_ENABLE_SHARC` 会在生成阶段过滤对应 entry。
RTDI 与 Hybrid direct-only composite 只受 `LUMIN_RAY_TRACING` 控制，不依赖 SHARC 或 NRD 的编译开关。

`.slang` 源码和其 `import`/`include` 依赖由 `slangc` depfile 在构建阶段跟踪，保存源码不会触发 CMake 重新配置；
Catalog 头、实现或生成器变化时才重新生成构建描述。生成内容未变化时会保留文件时间戳，
并且每个编译命令只依赖自身源码，因此普通配置刷新和单个 shader 修改都不会导致全量 shader 重编译；
shader ABI 校验同样只在 manifest、reflection 或校验器发生变化后运行。

项目自有的稳定 Slang 模块使用 `import`；厂商头、SHARC 头和依赖调用方宏特化的文本包装器保留 `#include`。
运行时由默认 pipeline session 持有唯一 `ShaderLibrary`，所有 Feature 使用 `ShaderId` 共享 shader handle 缓存。

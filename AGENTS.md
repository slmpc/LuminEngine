# AGENTS.md

本文件适用于仓库根目录及其所有子目录。

## 项目概览

Lumin Engine 是一个使用 C++23、Vulkan 1.3、SDL3 和 Slang 构建的紧凑型渲染器与场景沙盒。
主要代码位于：

- `core`：不依赖图形 API 的场景、资产、脚本和 Game API。
- `render`：独立渲染模块及其编辑器 UI。
- `application`：连接 Core 与 Render 的宿主组合层。
- `apps/sandbox`：可运行的示例程序。
- `shaders`：Slang 着色器源码。
- `tests`：引擎测试。
- `docs`：架构文档。

## 构建与测试

项目要求 CMake 3.25 或更高版本、Ninja、支持清单模式的 vcpkg，以及提供 `slangc` 的 Vulkan SDK。
如果 CMake 无法自动找到 vcpkg，请设置 `VCPKG_ROOT`。

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --test-dir out/build/debug --output-on-failure
```

可使用以下命令运行沙盒：

```powershell
.\out\build\debug\LuminEngine.exe
```

提交代码前，至少构建受影响的目标并运行相关测试。修改渲染流程、着色器或资源生命周期时，应同时运行完整测试集；具备可用 Vulkan 环境时，还应启动沙盒进行验证。

## 代码规范

- 使用 C++23，并遵循仓库根目录的 `.clang-format`。
- 使用 4 个空格缩进，禁止制表符；每行不超过 120 个字符。
- 头文件与对应实现统一位于 `src`，使用不带项目名称前缀的 include 路径（例如 `render/resources/FrameGraph.hpp`）。
- 优先沿用现有命名、所有权和错误处理方式，避免无关重构。
- 新增或移动源文件时，同步更新 `CMakeLists.txt`。
- 不要提交 `out`、`build`、生成的 SPIR-V、可执行文件或其他构建产物。

## 渲染与资源约束

- 图形通道使用 Vulkan 1.3 动态渲染；不要引入 `VkRenderPass` 或 framebuffer，除非项目架构明确调整。
- 通过 `FrameGraph` 声明纹理布局、流水线阶段及访问掩码，跨帧资源必须保留正确的初始同步状态。
- 帧槽资源只能在 `VulkanContext::beginFrame` 等待相应 fence 后更新。
- 修改 TAA、运动矢量、交换链重建或场景拓扑时，必须检查时序历史的失效规则。
- 着色器源码保存在 `shaders`，编译后的 SPIR-V 由 CMake 写入构建目录。

## 文档与测试要求

- 用户可见文档使用简体中文；命令、路径、代码标识符和 API 名称保持原样。
- 行为变化应同步更新 `README.md` 或 `docs` 下对应说明。
- 为可独立验证的场景、Actor 生命周期、地形、批处理或修订号逻辑补充测试。
- 测试应可重复运行，不依赖未纳入仓库的本地资源。

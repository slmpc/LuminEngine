# 第三方渲染依赖

## 固定版本

| 依赖 | 仓库路径 | 上游版本 | 固定 commit | 用途 |
| --- | --- | --- | --- | --- |
| SHARC | `thirdparty/sharc` | `v1.6.5.0` | `0b9f58bbc8c41736042d4da964830a247e424a00` | 世界空间辐射缓存 shader headers |
| NRD | `thirdparty/nrd` | `v4.17.3` | `792eff196afdd350fd9c3f862119017ccb438a0e` | REBLUR/RELAX 去噪 runtime 与 shaders |
| ShaderMake | `thirdparty/shadermake` | NRD 固定依赖 | `18f5a344e7ca8fa65daaf079d07bc8ce38453e05` | 离线生成 NRD shader blobs |
| MathLib | `thirdparty/mathlib` | `v11` | `974e1387ba936740c7cdc494792d2641bc127e86` | NRD CPU/shader 数学公共库 |

首次获取仓库后执行：

```powershell
git submodule update --init --recursive
```

`.gitmodules` 不记录跟踪分支。普通构建和 CI 禁止使用 `git submodule update --remote`，以免依赖在未审查时漂移。

## 构建边界

- `Lumin::Sharc` 只暴露 `thirdparty/sharc/include`，引擎公共头不得包含 vendor 类型。
- `Lumin::Nrd` 封装静态 `NRD` target；NRI 保持关闭，Vulkan 资源、queue 和 barrier 仍由 NvRHI 与
  Lumin `FrameGraph` 管理。
- NRD 只生成 SPIR-V blobs，DXIL、DXBC 和 NRI backend 默认关闭。
- ShaderMake 与 MathLib 在 NRD 之前从本地 submodule 建立 target，configure 阶段不会通过
  `FetchContent` 下载依赖。
- SHARC/NRD adapter 必须位于引擎实现目标内，vendor ABI、宏和 binding offset 不得进入
  `src` 的跨模块接口。

## 许可证边界

SHARC 与 NRD 使用各自仓库中的 NVIDIA RTX SDK License，不是开源许可证。分发前必须检查至少以下事项：

- 应用必须具有 SDK 之外的实质功能；不得将 SDK 作为独立产品分发。
- 修改或衍生的 NVIDIA sample source 需要包含许可证要求的 NVIDIA source notice。
- 分发条款必须满足上游对 SDK、知识产权、隐私和安全的保护要求。
- 不得用会强制披露或重新许可 SDK 的许可证组合方式分发受限 SDK 内容。

ShaderMake 与 MathLib 使用 MIT License。二进制或源码分发时应保留各自版权与许可文本。

本文件只记录工程约束，不构成法律意见。Lumin Engine 当前根 `LICENSE` 不是标准许可证；公开分发包含
SHARC/NRD 的构建前，必须先明确项目许可证并完成正式许可证兼容性审查。

## 受控升级

升级任一依赖时执行以下流程：

1. 核对上游 tag、commit、许可证和发布说明，记录 breaking changes。
2. 在对应 submodule 内 fetch 并 checkout 精确 commit，不写入 branch 跟踪配置。
3. 更新本文件和 `docs/rendering-overhaul-plan.md` 中的版本表。
4. 重新生成并验证 NRD shader blobs，运行 shader ABI、adapter、历史失效和完整渲染测试。
5. 在具备 RT 的 Vulkan 设备上运行 sandbox，检查 validation、SHARC overflow 和 NRD dispatch 诊断。

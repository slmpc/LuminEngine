# Shader 构建与 ABI

Slang 着色器二进制与 reflection JSON 由 CMake 生成到
`out/build/<preset>/generated/shaders`。这些文件属于构建产物，请勿提交 SPIR-V、reflection JSON 或 depfile。

每个 Slang 入口源文件都有一个同名 companion JSON，例如 `RtDi.slang` 配套 `RtDi.json`。shader 源文件、
companion JSON 和生成产物的 stem 统一使用大驼峰。
配置文件只描述这个源文件的入口，必须声明：

- Slang 源文件、entry point、stage、SPIR-V 输出和 reflection 输出；
- 需要显式启用的 capability；
- descriptor set、binding、资源类型以及参与校验的结构布局。

所有公共 ABI 和默认 `slangc` 编译选项集中在 `shader-abi.json`。这不是入口清单；构建时
`scripts/shader_manifest.py` 会扫描这些小配置，校验重复输出和依赖 feature，并在构建目录生成
临时的 `shader-manifest.json` 与 `shader-targets.cmake`。源码目录不再维护巨型总 manifest。

CMake 对每个 entry 启用 `-warnings-as-errors all`，并把 Slang `-depfile` 交给 Ninja。修改
`shaders/include` 下的公共文件会只重编依赖该文件的 shader。`gbuffer.fragment` 使用的额外 SPIR-V
capability 在 manifest 中逐项列出；不要使用 `-ignore-capabilities` 或全局关闭 capability 诊断。

`include/PostProcessUniforms.slang` 是全屏通道共享的 GPU ABI 定义。其布局必须与 CPU
`lumin::render::PostProcessUniforms` 保持一致：总大小为 512 字节，字段 offset 由生成 manifest、Slang
reflection 校验和 `ShaderCpuAbi` 测试共同锁定。

可单独构建并重复运行 ABI 校验：

```powershell
cmake --build --preset debug --target lumin_shader_abi
ctest --test-dir out/build/debug -R "Shader(Cpu|Reflection)Abi" --output-on-failure
```

当前延迟渲染 shader 包含 `Shadow`、`GBuffer`、`ao/AmbientOcclusion`、`Sky`、`Deferred`、`Taa`、
`PostProcess` 和 `ImGui`。

屏幕空间 AO 位于 `shaders/ao`：`AoCommon.slang` 提供共享资源与投影辅助函数，`Ssao.slang`、`Hbao.slang`、
`Gtao.slang` 分别实现算法，`AmbientOcclusion.slang` 只保留 shader 入口和运行时分派。

# Shader 构建与 ABI

Slang 着色器二进制与 reflection JSON 由 CMake 生成到
`out/build/<preset>/generated/shaders`。这些文件属于构建产物，请勿提交 SPIR-V、reflection JSON 或 depfile。

`shader-manifest.json` 是 shader 构建与 ABI 的机器可读清单。每个 entry 都必须声明：

- Slang 源文件、entry point、stage、SPIR-V 输出和 reflection 输出；
- 需要显式启用的 capability；
- descriptor set、binding、资源类型以及参与校验的结构布局。

CMake 对每个 entry 启用 `-warnings-as-errors all`，并把 Slang `-depfile` 交给 Ninja。修改
`shaders/include` 下的公共文件会只重编依赖该文件的 shader。`gbuffer.fragment` 使用的额外 SPIR-V
capability 在 manifest 中逐项列出；不要使用 `-ignore-capabilities` 或全局关闭 capability 诊断。

`include/PostProcessUniforms.slang` 是全屏通道共享的 GPU ABI 定义。其布局必须与 CPU
`lumin::render::PostProcessUniforms` 保持一致：总大小为 496 字节，字段 offset 由 manifest、Slang
reflection 校验和 `ShaderCpuAbi` 测试共同锁定。

可单独构建并重复运行 ABI 校验：

```powershell
cmake --build --preset debug --target lumin_shader_abi
ctest --test-dir out/build/debug -R "Shader(Cpu|Reflection)Abi" --output-on-failure
```

当前延迟渲染 shader 包含 `shadow`、`gbuffer`、`ssao`、`sky`、`deferred`、`taa`、`postprocess`
和 `imgui`。

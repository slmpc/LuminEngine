# Shader 构建与 ABI

Slang 着色器二进制与 reflection JSON 由 CMake 生成到
`out/build/<preset>/generated/shaders`。这些文件属于构建产物，请勿提交 SPIR-V、reflection JSON 或 depfile。

shader 的入口、stage、feature 条件、capability、binding 与 ABI 结构统一由
`render/resources/ShaderCatalog.cpp` 中的 `ShaderCatalogBuilder` 配置。新增入口时应先扩展 `ShaderId`，再通过
`ShaderCatalogBuilder::module()` 和 `ShaderModuleBuilder::entry()` 注册：

- Slang 源文件、entry point、stage、SPIR-V 输出和 reflection 输出；
- 需要显式启用的 capability；
- descriptor set、binding、资源类型以及参与校验的结构布局。

配置阶段会编译并运行纯 C++ 的 `ShaderCatalogGenerator`，在构建目录生成临时的 `shader-manifest.json` 与
`shader-targets.cmake`。前者只供反射 ABI 校验，后者注册逐入口 `slangc` 命令；二者都是构建产物。源码目录不保存
手写 shader JSON，也不依赖 Python。Catalog 会在生成前校验 ID、名称、输出路径和 ABI 名称的唯一性。

CMake 对每个 entry 启用 `-warnings-as-errors all`，并把 Slang `-depfile` 交给 Ninja。修改
`shaders/include` 下的公共文件会只重编依赖该文件的 shader。`gbuffer.fragment` 使用的额外 SPIR-V
capability 在 manifest 中逐项列出；不要使用 `-ignore-capabilities` 或全局关闭 capability 诊断。

项目自有的可复用 `.slang` 模块优先使用 `import`，模块路径由 `shaders` 与 `shaders/include` 搜索目录解析。
厂商 `NRD.hlsli`、SHARC C/HLSL 头，以及依赖调用方宏展开的 `SharcRuntime.slang` 保留
`#include`。不要把需要调用方预处理状态的文本包装器强行改成模块。

`include/PostProcessUniforms.slang` 是全屏通道共享的 GPU ABI 定义。其布局必须与 CPU
`lumin::render::PostProcessUniforms` 保持一致：总大小为 528 字节，字段 offset 由 C++ Catalog、Slang
reflection 校验和 `ShaderCpuAbi` 测试共同锁定。

`Bloom.slang` 使用 32 字节的 `BloomPushConstants`：`filter` 保存 source texel size、threshold 与 soft-knee，
`controls` 保存 intensity、radius 和 pass mode。CPU/Slang 布局同样由 Catalog reflection 与 `ShaderCpuAbi` 锁定。

可单独构建并重复运行 ABI 校验：

```powershell
cmake --build --preset debug --target lumin_shader_abi
ctest --test-dir out/build/debug -R "Shader(Cpu|Reflection)Abi" --output-on-failure
```

当前延迟渲染 shader 包含 `Shadow`、`GBuffer`、`ao/AmbientOcclusion`、`Sky`、`Deferred`、`Taa`、
`Bloom`、`PostProcess` 和 `ImGui`。

`Bloom` 在 TAA 后以六次 13-tap downsample、五次 tent upsample 和一次 HDR composite 构建多级泛光；仅第一级
应用 threshold/soft-knee。关闭时不记录这些图形 pass，而由 FrameGraph transfer pass 输出 TAA 直通副本。

`PostProcess` 默认使用参考 Alpha-Piscium 的 AgX inset/outset、16.5 EV log2 范围与默认对比度近似，并允许通过 uniform
回退到 ACES Filmic。`include/Fsr1Rcas.slang` 是 AMD FidelityFX FSR1 RCAS 的 Slang 移植；它在 TAA/Bloom 之后、sRGB
编码之前，对当前 tone-mapping 曲线产生的线性显示颜色执行五点对比度自适应锐化，TAA 历史始终保持未锐化。

屏幕空间 AO 位于 `shaders/ao`：`AoCommon.slang` 提供共享资源与投影辅助函数，`Ssao.slang`、`Hbao.slang`、
`Gtao.slang` 分别实现算法，`AmbientOcclusion.slang` 只保留 shader 入口和运行时分派。

运行时只通过 `ShaderLibrary::load(ShaderId)` 访问 SPIR-V。默认 pipeline session 持有唯一 `ShaderLibrary`，同一 ID
只读盘并创建一次 NvRHI shader，所有 Feature 共享其 `ShaderHandle` 缓存。

# 渲染测试与硬件跳过策略

Lumin Engine 将纯 CPU 契约测试与真实 Vulkan 硬件测试分开注册。能力探测通过不等于渲染链路通过；
`Vulkan13HardwareProbe`、`RayTracingHardwareProbe` 和 `SharcCapabilityHardwareProbe` 只证明当前设备满足
后续 smoke test 的前置条件，不能替代 RT hit/miss、SHARC 收敛或 NRD 时序输出验证。

## 标签

| 标签 | 含义 |
| --- | --- |
| `cpu` | 不创建设备、可在无 GPU 环境重复执行的单元、ABI 或契约测试。 |
| `vulkan` | Vulkan/NvRHI 相关契约或真实 Vulkan probe；可与 `hardware` 联合筛选真实设备测试。 |
| `raytracing` | RT 构建策略、能力、GPU Scene、shader ABI 或硬件 RT 测试。 |
| `sharc` | SHARC capability、adapter、cache 历史或运行时 smoke test。 |
| `nrd` | NRD adapter、资源翻译、历史或运行时 smoke test。 |
| `hardware` | 需要本机 Vulkan 设备的测试；设备不满足前置条件时允许跳过。 |
| `fallback` | 验证 `AUTO`/`OFF` 不创建 RT 资源并回退到 raster/SSAO 的测试。 |
| `smoke` | 对真实运行环境执行的最小端到端检查。 |

常用命令：

```powershell
# 可重复、与硬件无关的快速集合
ctest --test-dir out/build/debug -L cpu --output-on-failure

# Vulkan 相关全部测试，包括真实硬件 probe
ctest --test-dir out/build/debug -L vulkan --output-on-failure

# 只运行真实设备测试
ctest --test-dir out/build/debug -L hardware --output-on-failure

# 分域验证
ctest --test-dir out/build/debug -L raytracing --output-on-failure
ctest --test-dir out/build/debug -L sharc --output-on-failure
ctest --test-dir out/build/debug -L nrd --output-on-failure

# CI 中排除真实设备依赖
ctest --test-dir out/build/debug -LE hardware --output-on-failure
```

## 跳过与超时

`lumin_register_test(... HARDWARE)` 使用以下统一策略：

- 退出码 `77` 表示当前机器缺少所需 loader、物理设备、扩展或 Feature，CTest 记录为 `Skipped`。
- 参数错误、Vulkan API 调用在满足前置条件后失败、输出不符合契约等情况必须返回非零失败码，不能伪装为跳过。
- 硬件测试默认超时为 120 秒，并使用 `lumin_gpu` resource lock，避免多个测试同时争用同一 GPU。
- 普通 CPU 测试默认超时为 30 秒；较长的集成测试应在注册时显式覆盖。

SHARC 与 NRD 测试应先运行 adapter 的纯 CPU 资源/dispatch 翻译测试，再运行带 `hardware` 的端到端 smoke。
设备不支持 RT 时，fallback 测试仍必须执行并证明 SSAO/raster 路径可用、RT extension 与 AS/SBT 资源均未创建。

构建期 feature gate 还应使用独立目录验证，避免已有 cache 掩盖依赖图错误：

```powershell
cmake -S . -B out/build/rt-off -G Ninja -DLUMIN_RAY_TRACING=OFF
cmake -S . -B out/build/sharc-off -G Ninja -DLUMIN_ENABLE_SHARC=OFF
cmake -S . -B out/build/nrd-off -G Ninja -DLUMIN_ENABLE_NRD=OFF
cmake --build out/build/rt-off --target lumin_sandbox lumin_shader_abi
cmake --build out/build/sharc-off --target lumin_sandbox lumin_shader_abi
cmake --build out/build/nrd-off --target lumin_sandbox lumin_shader_abi
```

三种配置必须分别排除不满足 `requires` 的 shader entries，并能在不引用已关闭 adapter 符号的情况下链接 sandbox。

## 完整验证门槛

发布前至少执行 Debug 与 Release 全量测试。具备 RT 设备时，还必须运行 `hardware` 集合、启动 sandbox，
并在 Vulkan validation layer 下分别覆盖 raster、raw RT GI、SHARC 与 NRD 路径。长时间历史稳定性可使用：

```powershell
ctest --test-dir out/build/debug -L hardware --repeat until-fail:20 --output-on-failure
```

只有 capability、shader ABI、FrameGraph barrier、实际 dispatch、历史失效和最终图像证据同时成立时，才可判定
混合 GI 路径完成。

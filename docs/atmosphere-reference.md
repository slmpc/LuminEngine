# 大气系统参考审计与 clean-room 边界

本文记录 Lumin Engine 大气系统的外部参考、许可证边界、坐标约定和 LUT 数据流。它是实现约束，
不是 Alpha-Piscium 源码的移植说明。

## 审计基线

本地参考仓库：

- 路径：`D:\Games\Minecraft\.minecraft\versions\26.1.2-Fabric-Shader\shaderpacks\Alpha-Piscium`
- 分支：`1.10/fsr3`
- commit：`0a8f784367b43f0d615a50744167ef42d5e821df`
- 审计日期：2026-08-10

重点检查的具体路径如下。路径均相对于上述本地参考仓库；这些文件仅用于行为审计，不进入 Lumin 的构建或分发：

| 参考路径 | 审计内容 |
| --- | --- |
| `README.md`、`LICENSE` | 项目级 GPLv3 声明及 `scripts` 目录的 MIT 例外 |
| `scripts/programs.main.kts` | begin/composite 阶段的 pass 顺序 |
| `shaders/shadesmith.json` | 持久化 Transmittance 与 Multi-scattering 的格式和尺寸 |
| `scripts/shaders.properties` | Sky-view 与 epipolar image 的尺寸、格式和层数 |
| `shaders/techniques/atmospherics/air/lut/Common.glsl` | LUT 坐标参数化及公开上游引用 |
| `shaders/techniques/atmospherics/air/lut/API.glsl` | LUT consumer 的可观察采样约定 |
| `shaders/techniques/atmospherics/air/lut/GenerateTransmittance.comp.glsl` | Transmittance 生成职责 |
| `shaders/techniques/atmospherics/air/lut/GenerateMultiSctr.comp.glsl` | Multi-scattering 生成职责 |
| `shaders/techniques/atmospherics/air/lut/GenerateSkyViewLUT.comp.glsl` | Sky-view 分层输出职责 |
| `shaders/techniques/atmospherics/air/SliceEndPoints.comp.glsl` | Epipolar 切片端点生成职责 |
| `shaders/techniques/atmospherics/air/EpipolarScattering.comp.glsl` | 屏幕空间散射稀疏积分职责 |
| `shaders/techniques/atmospherics/air/UnwarpEpipolar.glsl` | Epipolar 数据展开与消费职责 |

## 许可证边界

Alpha-Piscium 的 `README.md` 明确声明：`scripts` 目录使用 MIT License，其余文件使用 GPLv3；仓库根
`LICENSE` 是 GPLv3 完整文本。个别 shader 注释列出 MIT/Apache-2.0 上游，并不自动把包含项目修改的该文件
重新许可为宽松许可证。
因此 Lumin Engine **不得复制、翻译或逐行改写**其大气 GLSL、绑定声明、常量布局或项目专用算法实现。
本次审计只记录下列不可版权化或可由公开资料独立验证的信息：

- pass 的职责与依赖关系；
- 纹理格式、分辨率和维度；
- 坐标系、单位及参数化的可观察行为；
- 资源更新频率和数值稳定问题；
- 文件中明确标注的上游论文与宽松许可证参考。

Lumin Engine 的 C++、Slang 和 `FrameGraph` 实现必须从公开论文及其允许复用的上游实现独立完成。优先参考：

- Sébastien Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique*, EGSR 2020；
- Epic Games 的 `UnrealEngineSkyAtmosphere` 示例，MIT License；
- Intel `OutdoorLightScattering` 示例，Apache-2.0；
- Eric Bruneton 的大气散射论文及其明确授权的参考实现。

当前 Lumin Engine 根 `LICENSE` 内容为非标准的 `All Rights Reversed.`。在发布或引入第三方源码前，
项目维护者必须明确最终许可证；本文不能替代法律审查。

## 参考拓扑

Alpha-Piscium 使用三类预计算 LUT 与一条屏幕空间 epipolar 链：

| 资源 | 参考格式与尺寸 | 输入依赖 | 主要用途 |
| --- | --- | --- | --- |
| Transmittance | `RGBA16F`, `256 x 64` | 行星半径、Rayleigh/Mie/ozone 介质 | 查询沿射线到大气边界的透射率 |
| Multi-scattering | `RGBA16F`, `32 x 32` | optical 参数、地表反照率、Transmittance | 近似多次散射能量 |
| Sky-view | `RGBA16F`, `N x N x 8`，默认 `N = 256` | optical、surface、太阳、相机高度及上游 LUT | 四个高度层，每层分别保存 in-scattering 与 transmittance |
| Epipolar data | `RGBA32UI`, `slices x (1 + 3 * samples)` | 当前视图、深度层、太阳和阴影 | 屏幕空间空气/水体散射的稀疏采样与展开 |

默认设置中 `slices = 1024`、`samples = 512`，epipolar 资源约为 24 MiB。该方案并不是传统的
camera-frustum 3D aerial-perspective LUT。Lumin Engine 第一版采用独立的 3D aerial-perspective LUT，
以便 raster、ray tracing miss shader 和 GI 共享；是否增加 epipolar 路径属于后续质量模式，不构成 ABI。

参考工程在 begin 阶段依次安排 Transmittance、Multi-scattering 和 Sky-view 生成，并在 composite 阶段执行
epipolar scattering 与 unwarp。Lumin Engine 不沿用其固定 pass 编号或 atlas binding，而由 Feature Pipeline
注册 pass、由 `FrameGraph` 独占资源状态转换。

## Lumin LUT 契约

Lumin Engine 采用四类输入 signature：

| LUT | Optical | Surface | Lighting | View |
| --- | ---: | ---: | ---: | ---: |
| Transmittance | 是 | 否 | 否 | 否 |
| Multi-scattering | 是 | 是 | 否 | 否 |
| Sky-view | 是 | 是 | 是 | 仅相机高度 |
| Aerial perspective | 是 | 是及上游结果 | 是 | 是 |

其中：

- `OpticalSignature` 包含行星半径、Rayleigh/Mie/ozone 密度分布和散射/吸收参数；
- `SurfaceSignature` 包含地表反照率及以后可能加入的地表辐射边界条件；
- `LightingSignature` 包含归一化太阳方向、颜色和照度；
- `ViewSignature` 包含相机行星高度、朝向、投影与渲染尺寸。

首次有效帧重建全部 LUT。输入变化只传播到表中依赖它的 LUT；上游 LUT 新 generation 同样使其消费者失效。
调度采用帧事务：`beginFrame()` 只产生计划，只有 GPU 工作成功提交后调用 `commitSubmittedFrame()` 才能推进
signature 和 generation；录制或提交失败必须调用 `abandonFrame()`，不得污染已提交历史。

四张 LUT 的当前数值与存储约定如下：

| LUT | 当前数值约定 |
| --- | --- |
| Transmittance | 使用 Bruneton/Hillaire 的边界距离参数化；RGB 为到大气上边界的透射率 |
| Multi-scattering | 对确定性 Fibonacci 球面方向积分；RGB 为单位太阳辐照度下的多次散射，A 为平均反馈率 |
| Sky-view | 经度覆盖完整方位角，纬度使用以地平线为中心的对称平方映射；RGB 为散射辐亮度，A 为平均透射率 |
| Aerial perspective | XY 对应相机视锥方向，Z 使用平方深度分布；RGB 为到该深度的累计散射，A 为平均透射率 |

Sky-view 和 aerial-perspective 在每个积分段内把介质与入射源视为常量，使用解析权重
`(1 - exp(-sigma_t * ds)) / sigma_t`；当 `sigma_t` 接近零时退化为 `ds`，避免除零和消减误差。
这属于标准辐射传输分段积分，不来自 Alpha-Piscium 的项目源码。所有方向样本均为确定性序列，Lumin 不沿用
参考工程的逐帧随机抖动或 temporal accumulation。

即使本帧没有 LUT 需要重建，`AtmosphereLutGpu::record()` 也必须在对应帧槽 fence 已等待后上传
`AtmosphereGpuConstants`。这些常量还会被 raster、RT 与 GI consumer 读取，不能把“无 compute pass”误认为
“无需初始化该帧槽”。

## 坐标与单位

Lumin Engine 使用 Y-up 世界坐标，场景单位通过 `kilometersPerWorldUnit` 显式转换为千米。
`seaLevelWorldY` 对应行星半径 `bottomRadiusKm`。局部平面世界中的相机位置映射为：

```text
altitudeKm = (cameraWorldY - seaLevelWorldY) * kilometersPerWorldUnit
cameraPlanetYKm = bottomRadiusKm + altitudeKm
```

水平坐标应相对稳定的局部原点转换，避免把巨大世界坐标直接装入 32 位 GPU 常量。

`DirectionalLight.direction` 始终表示光线传播方向；大气散射使用的朝日方向为：

```text
toSunWorld = normalize(-DirectionalLight.direction)
```

Sky-view 经度映射的行为基线为 `atan(direction.x, -direction.z) / (2 * pi) + 0.5`：负 Z 是经度中心，
正 Z 是接缝。参考工程的旧 API 注释曾与实际实现冲突，Lumin 的测试必须以正反映射行为为准。

Sky-view 纬度不是线性映射。令 `s = 1 - 2v`，生成方向时使用
`latitude = sign(s) * s^2 * pi/2`；采样时使用其解析逆映射。这样在固定分辨率下优先保留地平线附近的
密度梯度，同时保证天顶、地平线和天底精确落在参数域端点或中心。

当地表以上的相机高于 `topRadiusKm` 时，LUT 不会把视点钳回大气顶层。球壳求交先得到进入介质的距离，
再只对大气内部的线段积分；未命中大气或尚未到达介质入口的 aerial 深度切片保持零散射、单位透射率。

## 不采用的项目专用假设

下列做法不会直接进入 Lumin Engine：

- Minecraft 世界单位、固定高度偏移或参考工程中的 `BOTTOM_OFFSET`；
- Iris/Shadesmith atlas、固定纹理槽和生成式 binding 宏；
- 未经设备能力验证的 subgroup 大小假设；
- LUT 无条件逐帧 temporal accumulation；
- 参考工程的云层切片布局、三层透明深度和水体专用打包；
- 参数变化时保留旧 LUT 历史的行为。

所有近地面 epsilon、采样数、LUT 分辨率和格式都属于 Lumin renderer 的质量配置，必须经过 Vulkan 格式能力检查，
并在修改时纳入对应 signature。运行时 barrier 只允许由 `FrameGraph` 根据声明的读写状态生成。

## 后续验证

实现阶段至少覆盖：

1. 非法半径、密度尺度、相位参数、太阳方向和世界单位比例被拒绝；
2. 太阳方向取反并归一化，相机位置正确转换为行星千米坐标；
3. optical、surface、lighting、view 分别触发最小 LUT 重建集合；
4. abandon 的帧不推进 generation，同一帧序号可安全重试；
5. 昼夜变化、相机高度变化和 resize 后 raster sky 与 RT miss 的方向/曝光一致；
6. Vulkan validation 下 LUT pass 没有未声明访问或手写 barrier。

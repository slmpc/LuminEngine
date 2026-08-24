# 项目与场景编辑系统

## 项目结构

通过 `File > New Project` 创建项目后，目录固定为：

```text
ProjectName/
  ProjectName.luminproject
  Content/
    Meshes/
    Textures/
    Scripts/
  Scenes/
    Main.lumin.scene
  .lumin/
    AssetRegistry.json
```

清单、场景和注册表都包含 `formatVersion`。引擎拒绝未知版本、绝对资源路径和逃出项目根目录的相对路径。
场景与注册表使用临时文件和备份替换保存，写入失败时不会用不完整 JSON 覆盖原文件。

## 文件系统资源

`Content Browser` 直接递归读取项目根目录，以左侧目录树、面包屑和右侧类型图标网格展示文件。OBJ Mesh、PNG/JPG
Texture 和 Lua Script 可位于项目内任意目录；引擎发现可读文件后会自动创建稳定 128 位 `AssetId`，资源格式在实际
加载或附着时验证。目录每秒自动检查一次，也可通过工具栏刷新按钮立即同步。

`.lumin/AssetRegistry.json` 保存资源路径、类型、大小和内容指纹。原路径内容修改保持 ID；外部移动或重命名仅在新旧
指纹唯一匹配时迁移 ID，匹配有歧义时创建新 ID 并记录诊断。外部删除会保留不可用注册记录，以便文件恢复后继续使用
原引用。旧 v1 注册表会在打开项目时保留已有 ID 并升级到 v2。场景引用资源 ID，运行时句柄不会写入磁盘。

项目清单、场景、`.lumin` 内容和未识别的普通文件在浏览器中可见但只读；目录只用于导航。符号链接不会递归，指向项目
外的文件不会注册为资产，临时 `.tmp`、`.bak` 和 `.importing` 文件不会展示。

被当前场景模型、材质或脚本组件引用的资源不能删除。已绑定脚本必须先从 Actor 移除才能重命名，以免热重载源路径
失效。模型资源可从 `Content Browser` 双击或拖到 Viewport 创建 Actor，纹理可拖到材质的 base color、normal 和
roughness 槽。缺失的 base color、normal 或 roughness 图像分别使用白色、平坦法线或白色粗糙度 fallback，因而
只设置部分纹理也能参与渲染。

OBJ 加载会读取 MTL，并按 `usemtl` 材质名稳定聚合网格。单材质 OBJ 创建一个 Actor；多材质 OBJ 会为每个材质分区
创建一个 Actor，并导入 `Kd`、`Ks`、`Ns`、`map_Kd`、`norm` 和 `map_Pr`。场景通过 `mesh` 资源 ID 与
`meshPart` 材质名共同引用分区，保存、重开和源文件重命名后仍能恢复正确几何。旧场景未包含 `meshPart` 时继续加载
资源的首个分区。

## Actor 与脚本

项目 Actor 拥有持久 ID、名称、Transform、Material、可选 Mesh、可选 Point/Spot 局部灯和有序组件列表。模型与光源
可以挂在同一个 Actor 上；Spot 使用 Actor 本地 `-Z` 作为传播方向，位置取自 Transform，scale 不影响灯光。
`Scene Hierarchy` 创建菜单可生成两类光源 Actor，`Details` 支持类型切换、启用、线性颜色、坎德拉强度、范围、Spot
内外锥半角和逐灯阴影。复制、删除和编辑灯光与普通 Actor 一样更新项目 dirty 状态。

Lua 脚本作为 `ActorComponent`
附着：按列表顺序执行 `on_spawn` 和 `on_tick`，销毁时逆序执行 `on_destroy`，随后执行 Actor 自身的 `onDestroy`。
禁用脚本只跳过 Tick，移除或销毁 Actor 时仍执行销毁回调。旧 `ScriptRuntime::spawn` 保留兼容，它会创建普通脚本宿主
Actor 并调用同一附着接口。

项目场景只保存带持久 ID 的通用 Actor。Actor JSON 的可选 `light` 对象以 `point` 或 `spot` 标记类型并保存全部灯光
参数；旧场景缺少该对象时按无局部灯加载。未注册的原生 C++ Actor、程序化网格和其他临时运行时对象不会写入项目文件。
新项目使用空场景，并重置相机、环境和完整项目设置。

## Project Settings

`Project Settings` 随场景保存在顶层 `projectSettings` 对象中，其中 `logicTickHz` 控制独立逻辑线程的固定 Tick
频率，`render` 保存项目渲染设置。逻辑频率默认是 `60 Hz`，有效范围为 `15-240 Hz`；加载文件和 Editor 命令都会
归一化到该范围，面板修改后不需要重启项目。关闭项目后 Runtime 恢复默认 `60 Hz`。

旧场景的顶层 `renderSettings` 仍可读取；下一次保存会迁移到 `projectSettings.render`。

## 项目导航与全局设置

没有项目时，编辑器只显示 `Project Navigator`，其中提供最近项目、`New Project...` 和 `Open Project...`。项目打开后
切换到 dock 工作区；`File > Close Project` 关闭当前会话并返回导航器，dirty 项目继续使用 Save、Discard、Cancel。

`File > Configuration` 可选择默认进入导航器或打开上次项目。`View` 菜单及各面板标题栏控制七个编辑器面板的可见性，
其中 `Project Settings` 用于项目级运行参数。
启动模式、上次项目、最近十个项目和面板可见性保存在 SDL 首选目录的 `engine-settings.json` 中，不写入项目文件。

## Viewport 输入

- 左键：CPU 射线拾取最近三角形并选择 Actor/Model。
- `W`、`E`、`R`：切换平移、旋转和缩放 Gizmo；工具栏可切换 Local/World。
- 右键：选择命中物体并打开上下文菜单，进入 `Details` 或删除对象。
- 中键：捕获相对鼠标并旋转相机；捕获期间使用 `WASD`、`Space`、`Left Ctrl` 平移。

新建、打开或退出 dirty 项目时会显示 Save、Discard、Cancel。最近十个有效项目保存在 SDL 应用首选目录，不写入仓库。

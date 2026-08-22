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
roughness 槽。三张贴图全部就绪前材质使用无纹理 fallback，避免编辑中的部分纹理集触发渲染失败。

## Actor 与脚本

项目 Actor 拥有持久 ID、名称、Transform、Material、可选 Mesh 和有序组件列表。Lua 脚本作为 `ActorComponent`
附着：按列表顺序执行 `on_spawn` 和 `on_tick`，销毁时逆序执行 `on_destroy`，随后执行 Actor 自身的 `onDestroy`。
禁用脚本只跳过 Tick，移除或销毁 Actor 时仍执行销毁回调。旧 `ScriptRuntime::spawn` 保留兼容，它会创建普通脚本宿主
Actor 并调用同一附着接口。

项目场景只保存带持久 ID 的通用 Actor。Sandbox 中未注册的原生 C++ Actor、程序化网格和其他临时运行时对象不会写入
项目文件；创建或打开项目会用项目场景替换当前演示场景。

## Viewport 输入

- 左键：CPU 射线拾取最近三角形并选择 Actor/Model。
- `W`、`E`、`R`：切换平移、旋转和缩放 Gizmo；工具栏可切换 Local/World。
- 右键：选择命中物体并打开上下文菜单，进入 `Details` 或删除对象。
- 中键：捕获相对鼠标并旋转相机；捕获期间使用 `WASD`、`Space`、`Left Ctrl` 平移。

新建、打开或退出 dirty 项目时会显示 Save、Discard、Cancel。最近十个有效项目保存在 SDL 应用首选目录，不写入仓库。

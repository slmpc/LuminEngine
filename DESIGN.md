# Lumin Editor 设计系统与交互契约

本文是 Lumin Engine 原生 Dear ImGui 编辑器的实现基线，适用于单窗口 SDL3 + Vulkan 1.3 桌面工作台。
它不是网页或营销界面规范；实现中的颜色、字号、间距、尺寸、圆角和动效必须引用本文 token，不能另加魔法值。

## 1. 氛围与身份

Lumin Editor 面向需要长时间观察场景、调试渲染和处理脚本错误的引擎开发者。整体气质应当安静、精确、
高密度，像一台可信的图形工作台，而不是展示产品卖点的仪表盘。

- `DESIGN_VARIANCE: 2`：遵循成熟桌面编辑器结构，仅在选择、焦点和状态反馈上保留品牌辨识度。
- `MOTION_INTENSITY: 1`：状态转换直接、克制；不使用装饰动画或滑入式面板。
- `VISUAL_DENSITY: 8`：同屏展示足够多的层级、属性和诊断信息，但保留稳定的行高与键盘焦点。
- 实时三维 `Viewport` 是唯一的视觉焦点和产品资产。其余界面使用中性炭灰 chrome，为场景颜色让路。
- 识别色采用低面积的薄荷青，状态色同时配合图标和文字，不让整个界面落入单一蓝紫色系。
- 信息组织依赖层级、对齐、色调变化和选择性分隔线，不使用卡片、卡片嵌套或悬浮页面区块。
- 禁止渐变、发光、装饰性光斑、大号展示字体、负字距、纯黑、纯白、胶囊按钮和过度圆角。
- `Scene Hierarchy`、`Inspector`、`Viewport`、`Render/GI Settings`、`Script Console` 是一级工作面板，
  不是互相嵌套的组件。

Figma 的借鉴范围仅限于：围绕彩色创作画布的中性工具 chrome、明确的键盘焦点语言和稀疏层级。
其营销页常见的巨大字号、胶囊、渐变和强对比纯黑白不进入本设计。

## 2. 颜色

### 2.1 语义颜色 token

所有值均为 sRGB `#RRGGBBAA`。C++ 实现应集中转换为 `ImVec4`，业务面板不得直接写十六进制或浮点颜色。

| Token | 值 | 用途 |
| --- | --- | --- |
| `Color_Transparent` | `#11131500` | 无可见填充的 ImGui 槽位 |
| `Color_Backdrop` | `#111315FF` | DockSpace 空区、应用最底层 |
| `Color_Window` | `#171A1DFF` | 主窗口、菜单栏、滚动槽 |
| `Color_Panel` | `#1C2024FF` | 停靠面板、选中标签内容区 |
| `Color_Raised` | `#23282CFF` | 弹出菜单、模态框、工具提示 |
| `Color_Control` | `#202529FF` | 输入框、按钮、未选标签 |
| `Color_ControlHover` | `#2A3136FF` | 可交互控件 hover |
| `Color_ControlActive` | `#343D42FF` | 按下、拖动、持续激活 |
| `Color_Border` | `#626D73FF` | 必要窗口边界、弹出层边界、滚动条 grab |
| `Color_Divider` | `#2C3237FF` | 面板、工具栏和属性组分隔 |
| `Color_TextPrimary` | `#EDF1F3FF` | 主要文本与值 |
| `Color_TextSecondary` | `#BBC3C8FF` | 标签、次级说明 |
| `Color_TextMuted` | `#909BA1FF` | 元数据、占位提示 |
| `Color_TextDisabled` | `#69737AFF` | 禁用内容 |
| `Color_Accent` | `#50C7A9FF` | 选中标记、勾选、主操作 |
| `Color_AccentHover` | `#6BD8BCFF` | 主操作 hover |
| `Color_AccentActive` | `#39AA8FFF` | 主操作 active、分隔条 active |
| `Color_AccentInk` | `#10231EFF` | 高亮填充上的文本与图标 |
| `Color_Focus` | `#76D0FFFF` | 键盘焦点轮廓、Docking 预览 |
| `Color_Selection` | `#264740FF` | 选中行和选中标签的低面积底色 |
| `Color_Success` | `#65C982FF` | 成功、运行中且健康 |
| `Color_Warning` | `#E5B85CFF` | 警告、等待、可能丢失历史 |
| `Color_Error` | `#F0747CFF` | 错误、失败、无效值 |
| `Color_Info` | `#72ACE6FF` | 信息诊断、链接式跳转 |
| `Color_ViewportClear` | `#0E1012FF` | Viewport 尚无有效帧时的清屏色 |
| `Color_ModalDim` | `#080A0BC7` | 模态层后的遮罩 |
| `Color_AxisX` | `#E46767FF` | X 轴与 X 分量 |
| `Color_AxisY` | `#69C47EFF` | Y 轴与 Y 分量 |
| `Color_AxisZ` | `#6799E8FF` | Z 轴与 Z 分量 |

`Color_AxisX`、`Color_AxisY`、`Color_AxisZ` 只用于向量分量和 gizmo，不承担成功、警告或错误语义。

### 2.2 ImGui 槽位映射

| ImGui 槽位 | Token |
| --- | --- |
| `ImGuiCol_Text` | `Color_TextPrimary` |
| `ImGuiCol_TextDisabled` | `Color_TextDisabled` |
| `ImGuiCol_WindowBg` | `Color_Window` |
| `ImGuiCol_ChildBg` | `Color_Panel` |
| `ImGuiCol_PopupBg` | `Color_Raised` |
| `ImGuiCol_Border` | `Color_Border` |
| `ImGuiCol_BorderShadow` | `Color_Transparent` |
| `ImGuiCol_FrameBg` | `Color_Control` |
| `ImGuiCol_FrameBgHovered` | `Color_ControlHover` |
| `ImGuiCol_FrameBgActive` | `Color_ControlActive` |
| `ImGuiCol_TitleBg`, `ImGuiCol_TitleBgCollapsed` | `Color_Window` |
| `ImGuiCol_TitleBgActive` | `Color_Panel` |
| `ImGuiCol_MenuBarBg` | `Color_Window` |
| `ImGuiCol_ScrollbarBg` | `Color_Window` |
| `ImGuiCol_ScrollbarGrab` | `Color_Border` |
| `ImGuiCol_ScrollbarGrabHovered` | `Color_TextDisabled` |
| `ImGuiCol_ScrollbarGrabActive` | `Color_TextMuted` |
| `ImGuiCol_CheckMark`, `ImGuiCol_SliderGrab` | `Color_Accent` |
| `ImGuiCol_SliderGrabActive` | `Color_AccentHover` |
| `ImGuiCol_Button`, `ImGuiCol_Tab`, `ImGuiCol_TabDimmed` | `Color_Control` |
| `ImGuiCol_ButtonHovered`, `ImGuiCol_TabHovered` | `Color_ControlHover` |
| `ImGuiCol_ButtonActive` | `Color_ControlActive` |
| `ImGuiCol_Header` | `Color_Selection` |
| `ImGuiCol_HeaderHovered` | `Color_ControlHover` |
| `ImGuiCol_HeaderActive` | `Color_ControlActive` |
| `ImGuiCol_TabSelected` | `Color_Panel` |
| `ImGuiCol_TabDimmedSelected` | `Color_ControlActive` |
| `ImGuiCol_Separator` | `Color_Divider` |
| `ImGuiCol_SeparatorHovered` | `Color_AccentActive` |
| `ImGuiCol_SeparatorActive` | `Color_Accent` |
| `ImGuiCol_ResizeGrip` | `Color_Transparent` |
| `ImGuiCol_ResizeGripHovered` | `Color_AccentActive` |
| `ImGuiCol_ResizeGripActive` | `Color_Accent` |
| `ImGuiCol_TextSelectedBg` | `Color_Selection` |
| `ImGuiCol_DragDropTarget` | `Color_Warning` |
| `ImGuiCol_DockingPreview`, `ImGuiCol_NavCursor` | `Color_Focus` |
| `ImGuiCol_DockingEmptyBg` | `Color_Backdrop` |
| `ImGuiCol_TableHeaderBg` | `Color_Window` |
| `ImGuiCol_TableBorderStrong` | `Color_Border` |
| `ImGuiCol_TableBorderLight` | `Color_Divider` |
| `ImGuiCol_TableRowBg` | `Color_Transparent` |
| `ImGuiCol_TableRowBgAlt` | `Color_Window` |
| `ImGuiCol_ModalWindowDimBg` | `Color_ModalDim` |

若当前 ImGui 版本仍使用旧枚举名，应映射到语义等价的槽位，不得因此改动 token 值。
控制台严重级别固定使用 `Color_Info`、`Color_Warning`、`Color_Error`；成功结果使用 `Color_Success`。

## 3. 字体

### 3.1 字体资源 token

字体文件必须随仓库或应用资源安装，不能依赖开发机恰好存在的系统字体。

| Token | 字体与字重 | 用途 |
| --- | --- | --- |
| `Font_UIRegular` | `Noto Sans CJK SC Regular`, `400` | 菜单、标签、正文、输入 |
| `Font_UIMedium` | `Noto Sans CJK SC Medium`, `500` | 面板标题、选中项、主命令 |
| `Font_MonoRegular` | `JetBrains Mono Regular`, `400`，合并 `Noto Sans Mono CJK SC Regular` | 控制台、路径、句柄、数值诊断 |
| `Font_Icons` | Lucide 单色图标字形，合并到 UI atlas 的独立私用区 | 工具按钮和状态图标 |

图标不得占用正文 Unicode 码位，也不得使用 emoji 代替确定性工具图标。

### 3.2 字号与行高 token

字号是逻辑像素，不随窗口宽度缩放；DPI 缩放在构建 font atlas 时统一应用。

| Token | 字号 | 行高 | 字体 | 用途 |
| --- | --- | --- | --- | --- |
| `Type_Caption` | `11px` | `16px` | `Font_UIRegular` | 辅助元数据、时间戳 |
| `Type_Status` | `12px` | `16px` | `Font_UIRegular` | 状态栏、紧凑提示 |
| `Type_Body` | `13px` | `20px` | `Font_UIRegular` | 常规 UI、属性标签和值 |
| `Type_PanelTitle` | `13px` | `20px` | `Font_UIMedium` | 面板标题、选中标签 |
| `Type_SectionTitle` | `14px` | `20px` | `Font_UIMedium` | Inspector 与设置分组标题 |
| `Type_Console` | `12px` | `16px` | `Font_MonoRegular` | 控制台输出与命令输入 |

不设置负字距或随 viewport 变化的字号。紧凑感来自行高、间距和对齐，不来自压扁文字。

### 3.3 CJK 与 glyph 覆盖

- UI atlas 必须覆盖 `U+0020-U+00FF`、`U+2000-U+206F`、`U+3000-U+303F`、`U+3040-U+30FF`、
  `U+3400-U+4DBF`、`U+4E00-U+9FFF`、`U+AC00-U+D7AF`、`U+FF00-U+FFEF` 和 `U+FFFD`。
- 可用 `ImFontGlyphRangesBuilder` 合并 `GetGlyphRangesChineseFull()` 与上述补充范围；正文与 monospace atlas 都要验证。
- 所有面板标题、Actor/Model 名称、脚本路径、诊断和命令以有效 UTF-8 进入 UI。无效序列在系统边界被替换为
  `U+FFFD` 并记录一次诊断，不能把无效字节直接交给 ImGui。
- 缺字不是可接受的静默降级。测试字符串必须同时包含简体中文、日文假名、韩文、全角标点和拉丁字符；
  出现 tofu 方框即为失败。
- 省略、测量、光标移动和换行按 Unicode code point 或 grapheme cluster 处理，不能在 UTF-8 字节中间切断。

## 4. 间距与布局

### 4.1 间距 token

除分隔线、焦点轮廓和字体栅格对齐外，所有布局值使用 `4px` 基准。

| Token | 值 |
| --- | --- |
| `Space_0` | `0px` |
| `Space_1` | `4px` |
| `Space_2` | `8px` |
| `Space_3` | `12px` |
| `Space_4` | `16px` |
| `Space_5` | `20px` |
| `Space_6` | `24px` |
| `Space_8` | `32px` |
| `Space_10` | `40px` |

### 4.2 固定度量 token

| Token | 值 | 约束 |
| --- | --- | --- |
| `Metric_MenuBarHeight` | `24px` | 固定，不滚动 |
| `Metric_ToolbarHeight` | `32px` | 固定，图标按钮垂直居中 |
| `Metric_StatusBarHeight` | `24px` | 固定，不因消息换行增高 |
| `Metric_PanelHeaderHeight` | `28px` | 固定，标题与局部工具共用一行 |
| `Metric_RowHeight` | `24px` | 树行、列表行、属性行 |
| `Metric_InputHeight` | `24px` | 单行输入、下拉、数值控件 |
| `Metric_Divider` | `1px` | 唯一常规可见分隔厚度 |
| `Metric_FocusRing` | `2px` | 键盘焦点轮廓 |
| `Metric_Icon` | `16px` | 工具和状态图标 |
| `Metric_IconButton` | `24px` | 图标按钮命中区 |
| `Metric_WindowPadding` | `8px` | 面板内容内边距 |
| `Metric_FramePaddingX` | `8px` | 输入和按钮水平内边距 |
| `Metric_FramePaddingY` | `4px` | 输入和按钮垂直内边距 |
| `Metric_ItemSpacingX` | `8px` | 同行项目间距 |
| `Metric_ItemSpacingY` | `4px` | 相邻行间距 |
| `Metric_Indent` | `16px` | 层级树每级缩进 |
| `Metric_SplitterHitArea` | `8px` | 覆盖在可见 divider 上的拖动命中区 |
| `Metric_ScrollbarWidth` | `12px` | 面板滚动条宽度 |
| `Metric_PropertyLabelWidth` | `120px` | Inspector 标签列默认宽度 |
| `Metric_TooltipMaxWidth` | `320px` | 工具提示最大宽度 |
| `Metric_WindowRounding` | `0px` | Docked 与主窗口 |
| `Metric_ChildRounding` | `0px` | 面板内容区 |
| `Metric_FrameRounding` | `2px` | 输入、按钮、切换控件 |
| `Metric_TabRounding` | `2px` | Dock 标签 |
| `Metric_GrabRounding` | `2px` | Slider grab |
| `Metric_PopupRounding` | `2px` | 菜单、tooltip、modal |
| `Metric_WindowBorder` | `1px` | 浮动窗口边界 |
| `Metric_ChildBorder` | `0px` | Docked 面板不加第二层边框 |
| `Metric_PopupBorder` | `1px` | 弹出层边界 |
| `Metric_FrameBorder` | `0px` | 控件依靠色调区分 |
| `Metric_MinWorkbenchWidth` | `1280px` | 完整默认布局最小宽度 |
| `Metric_MinWorkbenchHeight` | `720px` | 完整默认布局最小高度 |
| `Metric_DefaultPropertiesWidth` | `320px` | 右侧 Scene/Details/Render 列 |
| `Metric_MinPropertiesWidth` | `280px` | 右侧面板缩放下限 |
| `Metric_DefaultConsoleHeight` | `200px` | 中央下方 Content Browser/Console 页签组 |
| `Metric_MinAuxPanelHeight` | `144px` | Scene、Details、Render、底部页签组下限 |
| `Metric_MinViewportWidth` | `480px` | 完整布局 Viewport 下限 |
| `Metric_MinViewportHeight` | `320px` | 完整布局 Viewport 下限 |
| `Metric_CompactViewportWidth` | `320px` | 窄窗口保留 Viewport 的下限 |
| `Metric_CompactViewportHeight` | `200px` | 窄窗口保留 Viewport 的下限 |
| `Metric_CompactBottomHeight` | `176px` | 紧凑模式底部标签组默认高度 |
| `Ratio_DefaultSceneHierarchyHeight` | `0.45` | 右列 Scene Hierarchy 默认高度比例 |
| `Ratio_DefaultPropertiesHeight` | `0.55` | 右列 Details/Render 页签组默认高度比例 |
| `Ratio_MaxSidePanel` | `0.40` | 单侧工具列最大客户区宽度比例 |
| `Ratio_MaxBottomPanel` | `0.45` | 底部工具区最大客户区高度比例 |

### 4.3 默认 Docking 布局

根窗口按顺序放置菜单栏、主工具栏、DockSpace 和状态栏；四者共享同一 OS 窗口，不启用独立平台窗口。
DockSpace 的完整默认布局如下：

```text
┌──────────────────────────── Viewport ──────────────┬── Scene Hierarchy ─┐
│                                                   ├────────────────────┤
├──────── Content Browser / Script Console ─────────┤ Details / Render GI│
└───────────────────────────────────────────────────┴────────────────────┘
```

- 左侧不创建工具列，`Viewport` 使用中央上方全部可用宽度；右列使用 `Metric_DefaultPropertiesWidth`。
- `Scene Hierarchy` 位于右上；`Details` 与 `Render / GI` 共享右下页签组，各辅助区域都受
  `Metric_MinAuxPanelHeight` 约束。
- `Content Browser` 与 `Script Console` 共享中央下方页签组并使用 `Metric_DefaultConsoleHeight`。
- `DockBuilder` 仅在首次运行、布局文件缺失或 layout schema 版本变化时创建默认节点，不能每帧重建。
- 布局状态保存在用户配置目录，不写入仓库。关闭的面板可从 `View` 菜单重新打开，并回到最后合法 dock 节点。
- Splitter 显示 `Metric_Divider`，命中区使用 `Metric_SplitterHitArea`。拖动时不预留空白，也不改变工具栏尺寸。
- 右侧面板不能超过 `Ratio_MaxSidePanel`，底部区域不能超过 `Ratio_MaxBottomPanel`；中央 Viewport 优先保留最小尺寸。

### 4.4 缩放、窄窗口与滚动所有权

- 客户区达到 `Metric_MinWorkbenchWidth` 与 `Metric_MinWorkbenchHeight` 时使用完整布局。
- 任一维度低于完整布局下限时进入紧凑布局；布局保持右侧上下分区与中央底部页签组，优先压缩右列和底部区域，
  不在 Viewport 左侧新增工具面板。
- 如果紧凑布局仍会让 Viewport 小于 `Metric_CompactViewportWidth` 或 `Metric_CompactViewportHeight`，
  两个辅助标签组默认折叠；它们仍可由 `View` 菜单作为 docked 标签打开，不能以遮住 Viewport 的浮动卡片替代。
- 工具栏保持单行；放不下的低优先级命令进入末端的 `More` 菜单。状态栏依次省略性能统计、后端详情和次级消息，
  但错误状态、busy 状态与当前选择始终可见。文本不得重叠。
- DockSpace 主体不拥有全局滚动。每个面板只滚动自己的内容区：层级树滚动树体；Inspector 滚动属性体；
  Render/GI 滚动设置体；Console 只滚动消息列表；菜单、面板标题、Console 命令输入和状态栏固定。
- Viewport 永不显示滚动条。其 Vulkan 图像按实际内容区的 framebuffer 像素尺寸重建，相机宽高比来自该尺寸。
- Panel resize 先执行各自下限，再执行最大比例；约束冲突时切换紧凑布局，不允许控件相互覆盖或掉出父窗口。

## 5. 组件与 primitives

### 5.1 复用 primitives

| Primitive | 职责 | 固定视觉与行为 |
| --- | --- | --- |
| `EditorShell` | 菜单、主工具栏、DockSpace、状态栏 | 使用 `Color_Window` 与 `Color_Backdrop`，尺寸引用 Section 4 token |
| `DockPanel` | 一级工作面板 | `Color_Panel`，只有 panel header 与内容区；禁止再包卡片 |
| `PanelToolbar` | 搜索、过滤、局部命令 | 使用 `Metric_PanelHeaderHeight` 与 `Metric_IconButton`，不单独抬升 |
| `IconButton` | 熟悉的单一工具命令 | Lucide 图标；陌生图标必须有 tooltip；无可见文本时仍有无障碍名称 |
| `CommandButton` | `Reload`、`Reset`、`Clear` 等明确命令 | 文本或图标加文本；主操作使用 `Color_Accent` 与 `Color_AccentInk`；不做胶囊形 |
| `SegmentedControl` | 互斥模式，如 gizmo 模式 | 共用一个外框；单项 selected，不用于多选过滤 |
| `Toggle` | 二元设置 | 同时显示状态文字；不只靠颜色表达开关 |
| `SearchField` | 层级和 Console 搜索 | 前置搜索图标、清除图标、UTF-8 输入 |
| `PropertyRow` | 标签、编辑器、重置/状态 | 使用固定 label 列和 input 高度；错误信息占下一逻辑行，不覆盖后续内容 |
| `Vector3Field` | Transform 和方向向量 | 三分量对齐，轴色仅用于短标签与 reset 图标 |
| `ColorField` | Material/light 色值 | 展示 swatch 和数值；颜色不是唯一信息 |
| `HierarchyRow` | Actor、Model、Script 节点 | 使用稳定 handle 的 index + generation 作为 ImGui ID |
| `StateBanner` | 面板级 warning/error/busy | 图标、短标题、可执行恢复命令；不铺满整面板高亮色 |
| `EmptyState` | 合法空集合或无选择 | 简短原因与唯一必要命令；不使用插画或装饰卡片 |
| `ConsoleLine` | 一条诊断或执行结果 | severity 图标、时间、phase/source、可复制正文 |
| `ConsoleCommand` | 固定在 Console 底部的命令输入 | 单行 UTF-8；历史导航；执行期间保持已提交文本可追溯 |
| `StatusSegment` | 选择、渲染后端、busy/error 状态 | 单行、可截断、带 tooltip，不改变状态栏高度 |
| `Tooltip` | 完整标签、原因、快捷说明 | 使用 `Color_Raised` 与 `Metric_TooltipMaxWidth` |
| `Splitter` | Dock 节点调整 | 可见 divider 与更宽命中区分离，拖动跟手 |

### 5.2 通用状态合同

| 状态 | 表现与规则 |
| --- | --- |
| `default` | 文本使用 primary/secondary token，控件使用 `Color_Control`；静态文本不得伪装成可点击控件 |
| `hover` | 只有可交互目标切换到 `Color_ControlHover`；光标和 tooltip 说明能力，不改变几何尺寸 |
| `active/selected` | 按下使用 `Color_ControlActive`；持续选择使用 `Color_Selection` 与 `Color_Accent` 标记 |
| `focus` | 使用 `Color_Focus` 和 `Metric_FocusRing` 的连续轮廓；与 hover、selected 可同时辨认 |
| `disabled` | 使用 `Color_TextDisabled`，停止 hover/active 反馈；tooltip 必须解释禁用原因 |
| `empty` | 使用 `EmptyState`；空集合和加载失败必须是不同文案与状态色 |
| `error` | 使用 error 图标、`Color_Error`、可复制错误文本和恢复动作；不能只把边框变红 |
| `busy` | 只禁用会冲突的 mutation，保留浏览、复制和滚动；状态栏与相关面板同时给出范围明确的进度文案 |

### 5.3 五个一级面板

#### Scene Hierarchy

- 根节点为当前 `Level`，下设 Actors、Models 和 Scripts 分组；缺少用户名称时显示类型与稳定句柄，不能伪造名称。
- 每行展示展开箭头、类型图标、名称/句柄和状态图标。搜索匹配名称、类型、句柄与脚本 source。
- 单击选择；方向键移动；左右键折叠/展开；Enter 激活；上下文菜单只提供当前 runtime 确实支持的命令。
- 每帧按 index + generation 校验选择。句柄失效时立即进入 stale-selection 流程，不允许 Inspector 写回复用后的槽位。
- 合法空 Level 使用 empty 状态；Level 正在 flush mutation 时显示 busy，但仍允许查看当前稳定快照。

#### Inspector

- 固定选择摘要位于顶部，滚动体依次展示 Transform、Material、Actor/Model 状态与 Script 状态。
- Transform 使用 `Vector3Field`；Material 颜色使用 `ColorField`；布尔值使用 `Toggle`；范围值优先 slider/drag，
  同时允许键盘精确输入。
- 编辑开始时记录选中 handle 与 revision；提交前再次校验。revision 或 generation 不匹配时放弃提交并显示 stale 状态。
- 可即时预览的标量在拖动时更新；资源、脚本 reload 和生命周期 mutation 只在明确提交后执行。
- 无选择时显示 empty 状态；部分能力不可用时只禁用相应行，并给出具体原因。

#### Viewport

- 显示真实场景 render target，不使用截图、渐变或占位插画替代。图像直接铺满 panel content，是界面视觉焦点。
- Panel toolbar 提供选择、移动、旋转、缩放模式的 segmented control，以及局部/世界坐标选项；命令使用标准图标与 tooltip。
- 空场景仍显示有效清屏和网格/天空结果；render target 尚未就绪时使用 `Color_ViewportClear` 与 busy 文案。
- resize 期间保留最后一张有效帧，直到新目标可用；错误时停止显示陈旧帧并给出 error 状态与重试命令。
- 性能统计放入状态栏，不在画面中央叠加说明卡片。选中轮廓和 gizmo 使用语义 token，不能污染场景色彩判断。

#### Render/GI Settings

- 分组为 Camera、Shadows、Global Illumination、Temporal AA、Tonemap 与 Lighting；组间只使用 divider 和 section title。
- 首版至少呈现 camera speed、Cascaded Shadows、Global Illumination、TAA、Exposure 与 Sun Direction，
  并展示当前 GI backend 名称、能力、temporal-history 状态和最近错误。
- 后端不支持的设置保持可见但 disabled，并在 tooltip 中说明能力缺失；不得静默隐藏导致用户误判当前配置。
- 会触发资源重建或历史失效的修改显示 busy，并在提交成功后刷新状态；失败时恢复最后有效值并保留错误详情。
- `Reset` 仅重置当前分组；恢复全部默认值属于独立、需确认的明确命令。

#### Script Console

- 消息列表消费 `ScriptDiagnostic.sequence`，展示 timestamp、severity、phase、script handle、source 与 message；
  Info、Warning、Error 使用独立 checkbox 过滤，搜索再作用于过滤结果。
- `ImGuiListClipper` 处理长历史。新消息只在用户原本位于底部时自动跟随；用户回看历史时不得抢走滚动位置。
- 固定底部 `ConsoleCommand` 调用 runtime `execute`。Enter 提交；Up/Down 只在输入获得 focus 时浏览 command history；
  busy 时拒绝重复提交但保留编辑和复制能力。
- `Clear Diagnostics` 与 `Clear Command History` 是两个独立命令。Reload Changed 显示每个脚本的结果，不用一条总成功覆盖部分失败。
- error 行可展开完整详情、复制 source/message，并能选择对应 Script；无诊断时显示合法 empty 状态。

## 6. 动效与交互

### 6.1 动效 token

| Token | 值 | 用途 |
| --- | --- | --- |
| `Motion_Instant` | `0ms` | hover、press、selection、focus 的几何与颜色切换 |
| `Motion_StateFade` | `80ms` | 非关键状态文字淡入淡出 |
| `Motion_ToastFade` | `120ms` | 非阻塞短消息进入与退出 |
| `Motion_BusyCycle` | `900ms` | 唯一允许循环的 busy 指示器 |
| `Motion_TooltipDelay` | `500ms` | 非错误 tooltip 延迟 |
| `Motion_StatusHold` | `3000ms` | 成功/信息状态最短保留时间 |

Docking、splitter、slider 和 gizmo 始终跟随指针，不做缓动。面板不滑入、不弹跳，hover 不改变尺寸。
启用 Reduce Motion 时，所有 fade 使用 `Motion_Instant`，busy 旋转替换为静态图标加动态文字。

### 6.2 键盘与焦点

- 启用 `ImGuiConfigFlags_NavEnableKeyboard`。Tab/Shift+Tab 按视觉顺序遍历当前面板，方向键处理树、菜单和 segmented control。
- 焦点进入面板时先到 panel toolbar，再到内容；Console 消息列表之后才到固定命令输入。Dock 标签也必须可由键盘切换。
- Enter/Space 激活当前命令；Escape 依次取消 active edit、关闭 popup/modal、退出 Viewport 导航，最后才允许应用层处理。
- 每个 icon-only 命令都有稳定可读名称和 tooltip。快捷键是加速路径，不得成为唯一访问方式。
- 打开 modal 后焦点被限制在 modal 内；关闭后返回触发控件。错误出现时不强抢正在输入的焦点。

### 6.3 SDL 与 ImGui 输入路由合同

以下顺序是系统约束，不得由面板自行变更：

1. 每个 SDL event 都先调用 `ImGui_ImplSDL3_ProcessEvent`，再进入窗口系统与 gameplay/editor 路由；
   `SDL_EVENT_QUIT`、窗口关闭、像素尺寸和焦点事件即使被 UI 捕获也必须继续由窗口系统处理。
2. ImGui frame 建立后读取 `ImGuiIO::WantCaptureKeyboard`、`ImGuiIO::WantCaptureMouse` 和
   `ImGuiIO::WantTextInput`。三者任一为真即定义 `uiClaimsInput`。
3. `uiClaimsInput` 为真时，`CameraController` 与所有 game input 获得零输入；清空相对鼠标 delta、滚轮和本帧按键边沿，
   不能因按键仍处于 held 状态继续移动相机。Escape 也先遵循 ImGui 的取消层级。
4. `uiClaimsInput` 为假时，game input 仍只在应用拥有 OS focus 时处理。失焦后立即清空 held/action 状态。
5. 不得以 `IsWindowHovered()`、透明窗口或手动重放事件绕过 capture flags。Viewport 的 orbit/pan/fly 若实现，
   必须作为显式的 editor command 由编辑器输入上下文处理；原始事件不再转发给 gameplay route。
6. `WantTextInput` 为真时启动 SDL text input/IME 路径；文本以 UTF-8 commit 事件进入当前输入控件，键码不能伪装成字符输入。

## 7. 深度与表面

- 层级只使用 `Color_Backdrop`、`Color_Window`、`Color_Panel`、`Color_Raised` 的色调变化，再辅以必要的
  `Color_Divider`。Docked panel 之间不绘制双边框。
- 一级面板是全宽/全高工作区，不画成悬浮卡片；属性分组也不包 child card，仅使用 section title、间距和 divider。
- `Viewport` 没有装饰边框、阴影或圆角遮罩，真实画面直接抵达 panel content 边缘。
- Popup、menu、tooltip 和 modal 使用 `Color_Raised`、`Metric_PopupBorder` 与 `Metric_PopupRounding`；不绘制自定义阴影。
- 浮动 Dock 窗口仅使用 `Metric_WindowBorder` 区分底层，不增加发光或大投影。主 docked 表面使用无圆角 token。
- 选中、focus、warning 和 error 的层级来自语义色与图标，不通过提高整块面板亮度制造“卡片浮起”效果。
- 任何表面都不得使用渐变、纹理噪声、装饰光斑或透明玻璃效果。

## 8. 无障碍约束与已接受债务

### 8.1 必须满足的约束

| Token | 值 | 适用范围 |
| --- | --- | --- |
| `A11y_TextContrast` | `4.5:1` | 常规文本与背景的最低对比度 |
| `A11y_LargeTextContrast` | `3:1` | 符合大文本条件时的最低对比度；本设计不依赖大文本豁免 |
| `A11y_UIContrast` | `3:1` | 图标、必要边界、focus 与状态标记的最低对比度 |

- Primary、secondary、muted 文本必须在实际承载表面上通过 `A11y_TextContrast`；disabled 虽属规范豁免，仍应保持可辨认。
- `Color_Divider` 只承担装饰分组，不作为控件或面板的唯一边界；必要边界必须使用通过 `A11y_UIContrast` 的
  `Color_Border`，低对比选择底色必须同时带 `Color_Accent` 状态标记。
- 键盘 focus 始终可见，不能仅靠 hover；selected 与 focus 必须同时成立且视觉上可区分。
- 成功、警告、错误、busy、脚本状态和坐标轴都同时使用文字或图标，不以颜色作为唯一线索。
- 所有常用任务都能只用键盘完成：打开面板、搜索层级、选择对象、编辑属性、切换渲染设置、查看/复制诊断、执行命令。
- Reduce Motion 必须可从 `View` 菜单切换并持久化；关闭动效后不丢失状态反馈。
- 初始 DPI 应从 SDL framebuffer scale 计算，字体与度量在同一比例下重建，避免只放大字体导致 clipping。
- CJK glyph、UTF-8、IME commit、复制和粘贴必须使用真实数据做人工验证，不能只检查 ASCII。

### 8.2 边界内容与故障行为

- **长标签**：层级行和单行属性标签在可用宽度内按 grapheme cluster 省略，hover/focus tooltip 展示完整内容；
  完整文本仍可复制。标签不能推挤输入框低于可操作宽度。
- **空数据**：空 Level、无选择、无诊断和无脚本分别使用具体 empty 文案；它们不等同于 loading，也不显示伪造示例行。
- **无断点诊断**：Console 默认按 Unicode 语义换行；优先在空白与路径标点处断开，CJK 遵循禁则，仍无断点时按
  grapheme cluster 软换行。底层 UTF-8 原文不变，复制得到完整消息。关闭 Wrap 时仅消息列表拥有水平滚动。
- **stale selection**：Actor/Model/Script 的 generation、revision 或 runtime 查询失效后，立即取消可写状态，
  Inspector 显示 stale banner，状态栏说明原因，并把诊断写入 Console；用户重新选择前禁止提交旧编辑。
- **busy**：显示具体操作名和作用域；只禁用冲突 mutation。超过正常帧时仍保持窗口事件、render、浏览、复制和取消响应。
- **error**：保留最后有效配置；展示 error code、phase、source 和可复制 message，提供重试、跳转或恢复动作之一。
  错误文案允许换行，绝不能覆盖后续控件。
- **CJK clipping/tofu**：所有固定高度控件用实际 atlas ascent/descent 验证垂直居中；出现裁顶、裁底、tofu 或半个 UTF-8
  字符均为阻塞缺陷。中文标点不置于行首，左括号不置于行尾，路径和代码保持可复制的原始字节序列。

### 8.3 首个 editor milestone 接受的债务

| 债务 | 受影响用户 | 当前边界与缓解 | 退出条件 |
| --- | --- | --- | --- |
| Dear ImGui 不提供 screen-reader accessibility tree | 盲人及依赖读屏的低视力用户 | 不宣称读屏可访问；关键错误同时写入可复制 Console 与进程日志 | 引入可访问语义桥或提供等价的原生可访问控制面 |
| 仅支持桌面键盘与鼠标 | 触控、笔、仅手柄用户 | 所有功能保留键盘路径；不显示未实现的触控提示 | 定义并实测 touch/gamepad 导航与命中目标 |
| `DockBuilder` 依赖 ImGui docking/internal API | 维护者与升级依赖者 | 封装在单一 layout builder，使用 schema version，并有默认布局回退 | 上游提供稳定 API，或替换为自有稳定布局层 |
| 跨显示器实时 DPI 切换尚不保证无闪烁重建 | 多显示器、不同缩放比例用户 | 启动时 DPI 正确；移动显示器后提供明确重载 UI 字体动作，绝不静默保持模糊 atlas | 自动检测 DPI 变化、重建 atlas，并通过跨屏截图与输入命中测试 |
| CJK IME 候选窗定位和复杂 composition 在所有平台组合上未验证 | 使用拼音、注音、日文或韩文 IME 的用户 | committed UTF-8、粘贴和显示必须工作；已知 composition 限制写入 release notes | 在 Windows/Linux 支持矩阵上验证候选窗、预编辑、提交、取消与焦点切换 |
| 单 OS 窗口，不启用 ImGui multi-viewport | 多显示器分离面板用户 | 所有五个面板可在主窗口内 docking、tabbing 和恢复 | 多窗口 Vulkan surface 生命周期、DPI 和输入路由通过完整 QA |

上述债务不能被描述为“已支持”。实现完成后必须使用真实 Vulkan 场景对完整布局、紧凑布局、键盘操作、
输入 capture、CJK、空/错误/busy/stale 状态进行截图与交互 QA；静态设计文档本身不构成视觉验收证据。

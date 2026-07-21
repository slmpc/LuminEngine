# Lumin 渲染层结构

这份文档记录当前渲染层的职责拆分，方便后续继续扩展 FrameGraph、材质系统和资源生命周期。

## FrameGraph

`FrameGraph` 负责 pass 排序、image layout barrier 和 buffer/image 的读写内存依赖。

渲染代码不直接调用 `vkCmdPipelineBarrier`。每个 pass 只通过 `FrameGraphBuilder::readTexture` 或 `writeTexture` 声明自己需要的目标 layout、pipeline stage 和 access mask；buffer pass 使用 `read`、`write` 或 `readWrite` 声明访问类型。`FrameGraph::execute` 会在 pass 执行前自动比较资源当前状态并录制 barrier。

当前 OBJ 渲染帧包含两个 pass：

- `OBJ + ImGui dynamic rendering`：把 swapchain image 转到 `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`，把 depth image 转到 `VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL`。
- `Present`：把 swapchain image 转到 `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`。

## Dynamic Rendering

项目不再创建 `VkRenderPass` 和 `VkFramebuffer`。图形 pipeline 通过 `VkPipelineRenderingCreateInfo` 声明 color/depth format；命令录制使用 `vkCmdBeginRendering` 和 `vkCmdEndRendering`。

`VulkanContext` 在 device 创建时启用 Vulkan 1.3 的 Dynamic Rendering feature。若设备不支持 Vulkan 1.3/Dynamic Rendering，初始化会直接失败。

## Pipeline 和 Shader

`ShaderLibrary` 负责从 CMake 生成的 shader 目录读取 SPIR-V 并创建 `VkShaderModule`。

`PipelineFactory` 负责把 shader、顶点布局、descriptor set layout、color/depth format 组装成 dynamic-rendering graphics pipeline。`ObjRenderer` 不再直接拼装 pipeline create info。

## 资源管理

`VulkanResourceManager` 封装 buffer、image、image view 和 device memory 的创建/销毁，并提供 depth format 和 memory type 查询。当前仍使用 host-visible buffer，后续可以在该类内部切换为 staging buffer + device-local buffer，而不影响上层 renderer。

## ImGui

`ImGuiLayerConfig` 集中描述 ImGui Vulkan backend 的配置项，包括 API version、queue、swapchain image count、color/depth format、descriptor pool 大小和输入选项。

ImGui backend 也使用 Dynamic Rendering，和主渲染 pipeline 一样不依赖 `VkRenderPass`。

# Lumin Rendering Architecture

## Scene Update

`Level` owns meshes, model instances, and Actors. Actor handles contain an index and generation so stale handles do not resolve after a slot is reused. Spawn and destroy requests made from `tick`, `onSpawn`, or `onDestroy` are deferred until the active callback traversal is safe to commit.

`Application` updates camera input, calls `Level::tick(deltaSeconds)`, and then renders. Model transform and material changes increment `modelRevision`; mesh/model membership changes increment `topologyRevision`. `LevelRenderer` uploads object records every frame and rebuilds packed geometry only when the topology revision changes.

`Terrain` generates an indexed height-field mesh with normalized accumulated triangle normals and bilinear height queries. `TerrainActor` owns the terrain data, attaches its generated mesh to the Level, and replaces the Level mesh after terrain edits.

## Frame Order

Each frame is recorded through `FrameGraph` in this order:

1. Four CSM depth-only passes, one independent 2D depth image per cascade.
2. G-buffer pass: world position, world normal and roughness, albedo, motion vector, and depth.
3. SSAO fullscreen pass reading world position and normal.
4. Procedural skybox fullscreen pass writing the HDR lighting target.
5. Deferred lighting pass loading that target and shading geometry with SSAO and CSM.
6. TAA resolve reading HDR lighting, motion, and the previous frame history.
7. Transfer copy from the resolved image into the current history image.
8. ACES tonemap to the swapchain.
9. ImGui overlay and presentation.

All graphics passes use Vulkan 1.3 dynamic rendering. `PipelineFactory` supports MRT pipelines and vertex-only depth pipelines; no `VkRenderPass` or framebuffer objects are created.

## Cascaded Shadows

The camera frustum is split into four logarithmic/uniform blended ranges. Each slice is fitted by an orthographic light projection and snapped to the shadow texel grid to reduce shimmering. Shadow matrices use four separate per-frame uniform buffers, so recording one cascade never overwrites data consumed by another cascade.

Shadow depth is sampled with explicit `Texture2D.Load` calls and a manual 3x3 PCF kernel. This avoids requiring linear filtering support for the selected depth format.

## Motion And TAA

`ObjectData` contains current and previous model matrices plus an inverse-transpose normal matrix for non-uniform scale. The G-buffer frame uniform contains current and previous jittered view-projection matrices. The motion attachment stores `currentUv - previousUv`; TAA reconstructs the previous sample location as `currentUv - motion`.

An eight-sample Halton (bases 2 and 3) sequence jitters the camera projection while TAA is enabled. Each frame slot writes its own history image and reads the other slot, producing true previous-frame ping-pong on the ordered graphics queue.

History is invalidated on first use, swapchain recreation, topology changes, camera cuts, meaningful FOV changes, and TAA off-to-on transitions. The first valid frame bypasses temporal blending. Content validity is tracked separately from whether each persistent history image has been initialized; invalidating a sample never discards its real shader-read layout/access state. FrameGraph can therefore emit the required shader-read to transfer-write dependency when that image is reused.

## Resource Ownership

`TextureManager` owns two frame slots. Each slot has its own G-buffer, SSAO, HDR lighting, TAA resolved/history, four shadow maps, postprocess uniform buffer, and descriptor set. `ModelRenderer` likewise owns per-frame object and camera buffers plus four per-frame shadow matrix buffers. A frame slot is updated only after `VulkanContext::beginFrame` has waited for its fence.

Swapchain recreation waits for device idle, shuts down ImGui, destroys pipelines before descriptor layouts/images, recreates extent-dependent resources, invalidates temporal history, and then initializes ImGui again.

## FrameGraph Contract

Pass setup callbacks declare texture layouts, pipeline stages, and access masks. `FrameGraph` derives ordering from read/write hazards and emits image or buffer barriers before each pass. Imported persistent textures may also supply `initialStages` and `initialAccess`; this is used by TAA history resources across submissions.

FrameGraph currently schedules and synchronizes externally allocated resources. It does not allocate transient images or alias memory. CSM therefore remains four single-layer images; an array-image implementation would also need explicit layer ranges in `FrameGraphTextureDesc`.

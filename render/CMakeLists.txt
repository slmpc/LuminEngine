# Render 由真实静态库组成；依赖只能从 Runtime/Feature 指向 Core、RHI 与 Vulkan 后端。
find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(nvrhi CONFIG REQUIRED)

function(lumin_configure_render_library target)
    lumin_configure_library(${target})
    target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${LUMIN_GENERATED_INCLUDE_DIR}>")
endfunction()

function(lumin_render_target_uses_shaders target)
    target_compile_definitions(${target} PRIVATE LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}")
    add_dependencies(${target} lumin_shaders)
endfunction()

add_library(lumin_render_core STATIC
    core/FrameDataContract.cpp
    core/History.cpp
    core/RenderCapabilities.cpp
    core/RenderFeaturePipeline.cpp
    core/RenderFeaturePlan.cpp
    core/RenderFeatureRegistry.cpp
    core/RenderPipelineInstance.cpp
    core/RenderSettingsStore.cpp
    world/RenderWorld.cpp
)
add_library(Lumin::RenderCore ALIAS lumin_render_core)
lumin_configure_render_library(lumin_render_core)
target_link_libraries(lumin_render_core PUBLIC Lumin::Core nvrhi)

add_library(lumin_render_rhi STATIC
    resources/DescriptorIndexingLimits.cpp
    resources/FrameGraph.cpp
    resources/FrameGraphResourceImporter.cpp
    resources/PipelineFactory.cpp
    resources/ShaderLibrary.cpp
    resources/VulkanResources.cpp
)
add_library(Lumin::RenderRhi ALIAS lumin_render_rhi)
lumin_configure_render_library(lumin_render_rhi)
target_link_libraries(lumin_render_rhi PUBLIC nvrhi)

add_library(lumin_vulkan_backend STATIC
    platform/RenderDocAttachment.cpp
    platform/Window.cpp
    platform/vulkan/VulkanContext.cpp
    platform/vulkan/VulkanRayTracingCapabilities.cpp
)
add_library(Lumin::VulkanBackend ALIAS lumin_vulkan_backend)
lumin_configure_render_library(lumin_vulkan_backend)
target_link_libraries(lumin_vulkan_backend
    PUBLIC Vulkan::Vulkan nvrhi nvrhi_vk SDL3::SDL3 Lumin::RenderCore
    PRIVATE ${CMAKE_DL_LIBS}
)

add_library(lumin_render_atmosphere STATIC
    atmosphere/AtmosphereGpuConstants.cpp
    atmosphere/AtmosphereLutGpu.cpp
    atmosphere/AtmosphereLutPassPlan.cpp
    atmosphere/AtmosphereLutResources.cpp
    atmosphere/AtmosphereLutScheduler.cpp
    atmosphere/AtmosphereTypes.cpp
)
add_library(Lumin::Atmosphere ALIAS lumin_render_atmosphere)
lumin_configure_render_library(lumin_render_atmosphere)
target_link_libraries(lumin_render_atmosphere PUBLIC Lumin::RenderCore Lumin::RenderRhi)
lumin_render_target_uses_shaders(lumin_render_atmosphere)

add_library(lumin_render_gpu_scene STATIC
    gpu/GpuScene.cpp
    gpu/GpuSceneResources.cpp
)
add_library(Lumin::GpuScene ALIAS lumin_render_gpu_scene)
lumin_configure_render_library(lumin_render_gpu_scene)
target_link_libraries(lumin_render_gpu_scene PUBLIC Lumin::RenderCore Lumin::RenderRhi)

add_library(lumin_render_raster STATIC
    ModelRenderer.cpp
    ObjRenderer.cpp
    resources/PipelineManager.cpp
    resources/TextureManager.cpp
)
add_library(Lumin::RasterFeatures ALIAS lumin_render_raster)
lumin_configure_render_library(lumin_render_raster)
target_link_libraries(lumin_render_raster
    PUBLIC Lumin::RenderCore Lumin::RenderRhi Lumin::VulkanBackend Lumin::GpuScene
)
lumin_render_target_uses_shaders(lumin_render_raster)

set(_lumin_gi_sources gi/legacy/LegacyBackend.cpp)
if(LUMIN_NRD_AVAILABLE)
    list(APPEND _lumin_gi_sources
        gi/raytracing/GiComposite.cpp
        gi/raytracing/HybridLightingComposite.cpp
        gi/raytracing/NrdDenoiser.cpp
        gi/raytracing/RayTracedDirectLighting.cpp
        gi/raytracing/RayTracedGi.cpp
    )
endif()
if(LUMIN_SHARC_AVAILABLE)
    list(APPEND _lumin_gi_sources gi/raytracing/SharcRadianceCache.cpp)
endif()
add_library(lumin_render_gi STATIC ${_lumin_gi_sources})
add_library(Lumin::GiFeatures ALIAS lumin_render_gi)
lumin_configure_render_library(lumin_render_gi)
target_link_libraries(lumin_render_gi PUBLIC Lumin::RenderCore Lumin::RenderRhi Lumin::Atmosphere Lumin::GpuScene)
if(LUMIN_NRD_AVAILABLE)
    target_link_libraries(lumin_render_gi PUBLIC Lumin::Nrd)
endif()
if(LUMIN_SHARC_AVAILABLE)
    target_link_libraries(lumin_render_gi PUBLIC Lumin::Sharc)
endif()
lumin_render_target_uses_shaders(lumin_render_gi)

add_library(lumin_render_pipelines STATIC
    DeferredRenderPipeline.cpp
    features/LevelRenderFeature.cpp
)
add_library(Lumin::RenderPipelines ALIAS lumin_render_pipelines)
lumin_configure_render_library(lumin_render_pipelines)
target_link_libraries(lumin_render_pipelines PUBLIC Lumin::RenderCore Lumin::RenderRhi)

add_library(lumin_render_presentation STATIC
    presentation/PresentationRenderer.cpp
    presentation/UiRenderer.cpp
)
add_library(Lumin::RenderPresentation ALIAS lumin_render_presentation)
lumin_configure_render_library(lumin_render_presentation)
target_link_libraries(lumin_render_presentation PUBLIC Lumin::RenderCore Lumin::RenderRhi Lumin::VulkanBackend)
lumin_render_target_uses_shaders(lumin_render_presentation)

add_library(lumin_render_features INTERFACE)
add_library(Lumin::RenderFeatures ALIAS lumin_render_features)
target_link_libraries(lumin_render_features INTERFACE
    Lumin::Atmosphere
    Lumin::GpuScene
    Lumin::RasterFeatures
    Lumin::GiFeatures
    Lumin::RenderPresentation
)

add_library(lumin_render_runtime STATIC
    LevelRenderer.cpp
    level/LevelRendererFeatures.cpp
    level/LevelRendererFrame.cpp
    level/LevelRendererResources.cpp
)
add_library(Lumin::RenderRuntime ALIAS lumin_render_runtime)
lumin_configure_render_library(lumin_render_runtime)
target_link_libraries(lumin_render_runtime
    PUBLIC Lumin::RenderCore Lumin::RenderRhi Lumin::VulkanBackend
    PRIVATE Lumin::RenderFeatures Lumin::RenderPipelines
)
lumin_render_target_uses_shaders(lumin_render_runtime)

add_library(lumin_editor_module STATIC
    editor/Editor.cpp
    editor/EditorStyle.cpp
    editor/ImGuiFrontend.cpp
    editor/ViewportPicking.cpp
)
add_library(Lumin::Editor ALIAS lumin_editor_module)
lumin_configure_render_library(lumin_editor_module)
target_link_libraries(lumin_editor_module
    PUBLIC Lumin::Core Lumin::RenderCore
    PRIVATE Lumin::VulkanBackend SDL3::SDL3 imgui::imgui imguizmo::imguizmo
)

# 默认门面组合 Runtime 与内置静态 Feature；它本身不再隐藏额外源码。
add_library(lumin_render INTERFACE)
add_library(Lumin::Render ALIAS lumin_render)
target_link_libraries(lumin_render INTERFACE Lumin::RenderRuntime Lumin::RenderFeatures Lumin::RenderPipelines)

set_target_properties(
    lumin_render_core lumin_render_rhi lumin_vulkan_backend lumin_render_atmosphere lumin_render_gpu_scene
    lumin_render_raster lumin_render_gi lumin_render_pipelines lumin_render_presentation lumin_render_runtime
    lumin_editor_module
    PROPERTIES FOLDER "Lumin/Render"
)

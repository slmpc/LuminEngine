# Render 是独立的 GPU 模块，依赖 Core 的场景快照，但不依赖 Application 或 Game。
find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(nvrhi CONFIG REQUIRED)

set(_lumin_render_sources
    ${PROJECT_SOURCE_DIR}/render/platform/RenderDocAttachment.cpp
    ${PROJECT_SOURCE_DIR}/render/platform/Window.cpp
    ${PROJECT_SOURCE_DIR}/render/resources/FrameGraph.cpp
    ${PROJECT_SOURCE_DIR}/render/platform/vulkan/VulkanContext.cpp
    ${PROJECT_SOURCE_DIR}/render/platform/vulkan/VulkanRayTracingCapabilities.cpp
    ${PROJECT_SOURCE_DIR}/render/resources/VulkanResources.cpp
    ${PROJECT_SOURCE_DIR}/render/DeferredRenderPipeline.cpp
    ${PROJECT_SOURCE_DIR}/render/resources/DescriptorIndexingLimits.cpp
    ${PROJECT_SOURCE_DIR}/render/ModelRenderer.cpp
    ${PROJECT_SOURCE_DIR}/render/resources/PipelineFactory.cpp
    ${PROJECT_SOURCE_DIR}/render/resources/PipelineManager.cpp
    ${PROJECT_SOURCE_DIR}/render/resources/ShaderLibrary.cpp
    ${PROJECT_SOURCE_DIR}/render/resources/TextureManager.cpp
    ${PROJECT_SOURCE_DIR}/render/LevelRenderer.cpp
    ${PROJECT_SOURCE_DIR}/render/level/LevelRendererResources.cpp
    ${PROJECT_SOURCE_DIR}/render/level/LevelRendererFeatures.cpp
    ${PROJECT_SOURCE_DIR}/render/level/LevelRendererFrame.cpp
    ${PROJECT_SOURCE_DIR}/render/features/LevelRenderFeature.cpp
    ${PROJECT_SOURCE_DIR}/render/ObjRenderer.cpp
    ${PROJECT_SOURCE_DIR}/render/editor/ImGuiLayer.cpp
    ${PROJECT_SOURCE_DIR}/render/editor/ImGuiManager.cpp
    ${PROJECT_SOURCE_DIR}/render/atmosphere/AtmosphereGpuConstants.cpp
    ${PROJECT_SOURCE_DIR}/render/atmosphere/AtmosphereLutGpu.cpp
    ${PROJECT_SOURCE_DIR}/render/atmosphere/AtmosphereLutPassPlan.cpp
    ${PROJECT_SOURCE_DIR}/render/atmosphere/AtmosphereLutResources.cpp
    ${PROJECT_SOURCE_DIR}/render/atmosphere/AtmosphereLutScheduler.cpp
    ${PROJECT_SOURCE_DIR}/render/atmosphere/AtmosphereTypes.cpp
    ${PROJECT_SOURCE_DIR}/render/core/History.cpp
    ${PROJECT_SOURCE_DIR}/render/core/RenderCapabilities.cpp
    ${PROJECT_SOURCE_DIR}/render/core/RenderFeaturePipeline.cpp
    ${PROJECT_SOURCE_DIR}/render/core/RenderFeaturePlan.cpp
    ${PROJECT_SOURCE_DIR}/render/gpu/GpuScene.cpp
    ${PROJECT_SOURCE_DIR}/render/gpu/GpuSceneResources.cpp
    ${PROJECT_SOURCE_DIR}/render/world/RenderWorld.cpp
    ${PROJECT_SOURCE_DIR}/render/gi/SsaoBackend.cpp
    ${PROJECT_SOURCE_DIR}/render/editor/Editor.cpp
    ${PROJECT_SOURCE_DIR}/render/editor/EditorStyle.cpp
    ${PROJECT_SOURCE_DIR}/render/editor/ViewportPicking.cpp
)

if(LUMIN_NRD_AVAILABLE)
    list(APPEND _lumin_render_sources
        ${PROJECT_SOURCE_DIR}/render/gi/GiComposite.cpp
        ${PROJECT_SOURCE_DIR}/render/gi/HybridLightingComposite.cpp
        ${PROJECT_SOURCE_DIR}/render/gi/NrdDenoiser.cpp
        ${PROJECT_SOURCE_DIR}/render/gi/RayTracedDirectLighting.cpp
        ${PROJECT_SOURCE_DIR}/render/gi/RayTracedGi.cpp
    )
endif()
if(LUMIN_SHARC_AVAILABLE)
    list(APPEND _lumin_render_sources ${PROJECT_SOURCE_DIR}/render/gi/SharcRadianceCache.cpp)
endif()

add_library(lumin_render STATIC ${_lumin_render_sources})
add_library(Lumin::Render ALIAS lumin_render)
add_library(Lumin::Rendering ALIAS lumin_render)
add_library(Lumin::RenderCore ALIAS lumin_render)
add_library(Lumin::VulkanBackend ALIAS lumin_render)
add_library(Lumin::RenderFeatures ALIAS lumin_render)
add_library(Lumin::Atmosphere ALIAS lumin_render)
add_library(Lumin::Editor ALIAS lumin_render)
lumin_configure_library(lumin_render)
target_include_directories(lumin_render
    PUBLIC
        "$<BUILD_INTERFACE:${LUMIN_GENERATED_INCLUDE_DIR}>"
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/core"
)
target_link_libraries(lumin_render
    PUBLIC
        Lumin::Core
        Vulkan::Vulkan
        nvrhi
        nvrhi_vk
        SDL3::SDL3
        imgui::imgui
        imguizmo::imguizmo
    PRIVATE
        ${CMAKE_DL_LIBS}
)
if(LUMIN_NRD_AVAILABLE)
    target_link_libraries(lumin_render PUBLIC Lumin::Nrd)
endif()
if(LUMIN_SHARC_AVAILABLE)
    target_link_libraries(lumin_render PUBLIC Lumin::Sharc)
endif()
target_compile_definitions(lumin_render PRIVATE LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}")
add_dependencies(lumin_render lumin_shaders)

# 旧 target 名仅保留 alias，避免测试和外部示例在迁移期间同时改动。
set_target_properties(lumin_render PROPERTIES EXPORT_NAME Render)

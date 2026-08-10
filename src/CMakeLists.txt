add_library(lumin_runtime STATIC)
add_library(Lumin::Runtime ALIAS lumin_runtime)
target_sources(lumin_runtime
    PRIVATE
        assets/ImageLoader.cpp
        assets/ObjLoader.cpp
        scene/Camera.cpp
        scene/CameraController.cpp
        scene/Environment.cpp
        scene/Level.cpp
)
lumin_configure_library(lumin_runtime)
target_include_directories(lumin_runtime PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(lumin_runtime
    PUBLIC
        glm::glm
    PRIVATE
        tinyobjloader::tinyobjloader
)

add_library(lumin_render_core STATIC)
add_library(Lumin::RenderCore ALIAS lumin_render_core)
target_sources(lumin_render_core
    PRIVATE
        render/FrameGraph.cpp
        render/core/History.cpp
        render/core/RenderCapabilities.cpp
        render/core/RenderFeaturePipeline.cpp
        render/core/RenderFeaturePlan.cpp
        render/gpu/GpuScene.cpp
        render/gpu/GpuSceneResources.cpp
        render/world/RenderWorld.cpp
)
lumin_configure_library(lumin_render_core)
target_link_libraries(lumin_render_core
    PUBLIC
        Lumin::Runtime
        nvrhi
)

add_library(lumin_atmosphere STATIC)
add_library(Lumin::Atmosphere ALIAS lumin_atmosphere)
target_sources(lumin_atmosphere
    PRIVATE
        render/atmosphere/AtmosphereGpuConstants.cpp
        render/atmosphere/AtmosphereLutPassPlan.cpp
        render/atmosphere/AtmosphereLutResources.cpp
        render/atmosphere/AtmosphereLutScheduler.cpp
        render/atmosphere/AtmosphereTypes.cpp
)
lumin_configure_library(lumin_atmosphere)
target_link_libraries(lumin_atmosphere
    PUBLIC
        Lumin::RenderCore
)

add_library(lumin_vulkan_backend STATIC)
add_library(Lumin::VulkanBackend ALIAS lumin_vulkan_backend)
target_sources(lumin_vulkan_backend
    PRIVATE
        platform/RenderDocAttachment.cpp
        platform/Window.cpp
        render/VulkanContext.cpp
        render/VulkanRayTracingCapabilities.cpp
        render/VulkanResources.cpp
)
lumin_configure_library(lumin_vulkan_backend)
target_include_directories(lumin_vulkan_backend
    PUBLIC
        "$<BUILD_INTERFACE:${LUMIN_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(lumin_vulkan_backend
    PUBLIC
        Vulkan::Vulkan
        nvrhi
        nvrhi_vk
    PRIVATE
        Lumin::Runtime
        SDL3::SDL3
        ${CMAKE_DL_LIBS}
)

set(_lumin_render_feature_sources
        render/DeferredRenderPipeline.cpp
        render/DescriptorIndexingLimits.cpp
        render/ModelRenderer.cpp
        render/PipelineFactory.cpp
        render/PipelineManager.cpp
        render/ShaderLibrary.cpp
        render/TextureManager.cpp
        render/atmosphere/AtmosphereLutGpu.cpp
        render/gi/SsaoBackend.cpp
)
if(LUMIN_NRD_AVAILABLE)
    list(APPEND _lumin_render_feature_sources
        render/gi/GiComposite.cpp
        render/gi/HybridLightingComposite.cpp
        render/gi/NrdDenoiser.cpp
        render/gi/RayTracedDirectLighting.cpp
    )
endif()
if(LUMIN_SHARC_AVAILABLE)
    list(APPEND _lumin_render_feature_sources render/gi/SharcRadianceCache.cpp)
endif()
# Raw RT GI 只依赖 NRD 前端信号格式；SHARC 通过可选 graph record 和 closest-hit 变体接入。
if(LUMIN_NRD_AVAILABLE)
    list(APPEND _lumin_render_feature_sources render/gi/RayTracedGi.cpp)
endif()

add_library(lumin_render_features STATIC)
add_library(Lumin::RenderFeatures ALIAS lumin_render_features)
target_sources(lumin_render_features PRIVATE ${_lumin_render_feature_sources})
lumin_configure_library(lumin_render_features)
target_include_directories(lumin_render_features PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(lumin_render_features
    PUBLIC
        Lumin::Atmosphere
        Lumin::RenderCore
        Lumin::Runtime
        Lumin::VulkanBackend
)
if(LUMIN_NRD_AVAILABLE)
    target_link_libraries(lumin_render_features PUBLIC Lumin::Nrd)
endif()
if(LUMIN_SHARC_AVAILABLE)
    target_link_libraries(lumin_render_features PUBLIC Lumin::Sharc)
endif()
add_dependencies(lumin_render_features lumin_shaders)

add_library(lumin_render_engine STATIC)
add_library(Lumin::Rendering ALIAS lumin_render_engine)
target_sources(lumin_render_engine
    PRIVATE
        render/ImGuiLayer.cpp
        render/ImGuiManager.cpp
        render/LevelRenderer.cpp
        render/ObjRenderer.cpp
)
lumin_configure_library(lumin_render_engine)
target_link_libraries(lumin_render_engine
    PUBLIC
        Lumin::RenderFeatures
    PRIVATE
        SDL3::SDL3
        imgui::imgui
)
target_compile_definitions(lumin_render_engine PRIVATE LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}")
add_dependencies(lumin_render_engine lumin_shaders)

add_library(lumin_scripting STATIC)
add_library(Lumin::Scripting ALIAS lumin_scripting)
target_sources(lumin_scripting PRIVATE scripting/ScriptRuntime.cpp)
lumin_configure_library(lumin_scripting)
target_include_directories(lumin_scripting PRIVATE "${LUA_INCLUDE_DIR}")
target_link_libraries(lumin_scripting
    PUBLIC
        Lumin::Runtime
    PRIVATE
        sol2::sol2
        ${LUA_LIBRARIES}
)

add_library(lumin_editor STATIC)
add_library(Lumin::Editor ALIAS lumin_editor)
target_sources(lumin_editor
    PRIVATE
        editor/Editor.cpp
        editor/EditorStyle.cpp
)
lumin_configure_library(lumin_editor)
target_link_libraries(lumin_editor
    PUBLIC
        Lumin::Rendering
        Lumin::Scripting
    PRIVATE
        imgui::imgui
)

add_library(lumin_game_engine STATIC)
add_library(Lumin::GameEngine ALIAS lumin_game_engine)
target_sources(lumin_game_engine PRIVATE core/Application.cpp)
lumin_configure_library(lumin_game_engine)
target_link_libraries(lumin_game_engine
    PUBLIC
        Lumin::Editor
        Lumin::Rendering
        Lumin::Scripting
)
target_compile_definitions(lumin_game_engine
    PRIVATE
        $<$<CONFIG:Debug>:LUMIN_ENABLE_VALIDATION>
        LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}"
)

add_library(lumin_engine INTERFACE)
add_library(Lumin::Engine ALIAS lumin_engine)
target_link_libraries(lumin_engine INTERFACE Lumin::GameEngine)

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(_lumin_vcpkg_candidates
        "$ENV{VCPKG_ROOT}"
        "D:/Programs/vcpkg"
        "D:/vcpkg"
        "C:/vcpkg"
    )

    foreach(_lumin_vcpkg_root IN LISTS _lumin_vcpkg_candidates)
        if(_lumin_vcpkg_root AND EXISTS "${_lumin_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
            file(TO_CMAKE_PATH "${_lumin_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" _lumin_vcpkg_toolchain)
            set(CMAKE_TOOLCHAIN_FILE "${_lumin_vcpkg_toolchain}" CACHE FILEPATH "vcpkg toolchain file")
            message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
            break()
        endif()
    endforeach()
endif()

project(LuminEngine VERSION 0.1.0 LANGUAGES CXX)

option(LUMIN_BUILD_SANDBOX "Build the sandbox executable." ON)
option(LUMIN_BUILD_TESTS "Build the engine tests." ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LuminShaders)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nvrhi CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(Stb REQUIRED)

set(LUMIN_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(LUMIN_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
set(LUMIN_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(TO_CMAKE_PATH "${LUMIN_SHADER_OUTPUT_DIR}" LUMIN_SHADER_OUTPUT_DIR_CMAKE)
file(TO_CMAKE_PATH "${LUMIN_ASSET_DIR}" LUMIN_ASSET_DIR_CMAKE)

set(LUMIN_SHADER_OUTPUTS)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/gbuffer.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/postprocess.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)
lumin_add_slang_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/shadow.slang"
    OUTPUT "${LUMIN_SHADER_OUTPUT_DIR}/shadow.vert.spv"
    ENTRY_POINT vertexMain
    STAGE vertex
    MATRIX_LAYOUT column-major
)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/ssao.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/deferred.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/sky.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/taa.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/imgui.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)

add_custom_target(lumin_shaders ALL
    DEPENDS ${LUMIN_SHADER_OUTPUTS}
)

function(lumin_configure_library target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PUBLIC /utf-8 PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(lumin_runtime STATIC)
add_library(Lumin::Runtime ALIAS lumin_runtime)
target_sources(lumin_runtime
    PRIVATE
        src/assets/ImageLoader.cpp
        src/assets/ObjLoader.cpp
        src/scene/Camera.cpp
        src/scene/CameraController.cpp
        src/scene/Level.cpp
)
lumin_configure_library(lumin_runtime)
target_include_directories(lumin_runtime PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(lumin_runtime
    PUBLIC
        glm::glm
    PRIVATE
        tinyobjloader::tinyobjloader
)

add_library(lumin_render_engine STATIC)
add_library(Lumin::Rendering ALIAS lumin_render_engine)
target_sources(lumin_render_engine
    PRIVATE
        src/platform/RenderDocAttachment.cpp
        src/platform/Window.cpp
        src/render/FrameGraph.cpp
        src/render/ImGuiLayer.cpp
        src/render/ImGuiManager.cpp
        src/render/LevelRenderer.cpp
        src/render/ModelRenderer.cpp
        src/render/ObjRenderer.cpp
        src/render/PipelineManager.cpp
        src/render/PipelineFactory.cpp
        src/render/ShaderLibrary.cpp
        src/render/TextureManager.cpp
        src/render/VulkanContext.cpp
        src/render/VulkanResources.cpp
)
lumin_configure_library(lumin_render_engine)
target_link_libraries(lumin_render_engine
    PUBLIC
        Lumin::Runtime
        Vulkan::Vulkan
        nvrhi
        nvrhi_vk
    PRIVATE
        SDL3::SDL3
        imgui::imgui
        ${CMAKE_DL_LIBS}
)
target_compile_definitions(lumin_render_engine PRIVATE LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}")
add_dependencies(lumin_render_engine lumin_shaders)

if(WIN32)
    target_compile_definitions(lumin_render_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(LUMIN_GAME_ENGINE_FEATURE_LIBRARIES)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuminGlobalIllumination.cmake" OPTIONAL)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuminScripting.cmake" OPTIONAL)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuminEditor.cmake" OPTIONAL)

add_library(lumin_game_engine STATIC)
add_library(Lumin::GameEngine ALIAS lumin_game_engine)
target_sources(lumin_game_engine PRIVATE src/core/Application.cpp)
lumin_configure_library(lumin_game_engine)
target_link_libraries(lumin_game_engine
    PUBLIC
        Lumin::Rendering
        ${LUMIN_GAME_ENGINE_FEATURE_LIBRARIES}
)
target_compile_definitions(lumin_game_engine
    PRIVATE
        $<$<CONFIG:Debug>:LUMIN_ENABLE_VALIDATION>
        LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}"
)

add_library(lumin_engine INTERFACE)
add_library(Lumin::Engine ALIAS lumin_engine)
target_link_libraries(lumin_engine INTERFACE Lumin::GameEngine)

if(LUMIN_BUILD_SANDBOX)
    add_executable(lumin_sandbox
        apps/sandbox/main.cpp
        apps/sandbox/SandboxGame.cpp
    )
    target_link_libraries(lumin_sandbox PRIVATE Lumin::GameEngine)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/apps/sandbox/scripts" LUMIN_SANDBOX_SCRIPT_DIR_CMAKE)
    target_compile_definitions(lumin_sandbox PRIVATE
        LUMIN_ASSET_DIR="${LUMIN_ASSET_DIR_CMAKE}"
        LUMIN_SANDBOX_SCRIPT_DIR="${LUMIN_SANDBOX_SCRIPT_DIR_CMAKE}"
    )
    add_dependencies(lumin_sandbox lumin_shaders)
    set_target_properties(lumin_sandbox PROPERTIES OUTPUT_NAME LuminEngine)
endif()

if(LUMIN_BUILD_TESTS)
    enable_testing()
    add_executable(lumin_nvrhi_dependency_tests tests/NvrhiDependencyTests.cpp)
    target_link_libraries(lumin_nvrhi_dependency_tests PRIVATE nvrhi nvrhi_vk)
    add_test(NAME NvrhiDependency COMMAND lumin_nvrhi_dependency_tests)

    add_executable(lumin_nvrhi_backend_tests tests/NvrhiBackendTests.cpp)
    target_include_directories(lumin_nvrhi_backend_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lumin_nvrhi_backend_tests PRIVATE Vulkan::Vulkan nvrhi nvrhi_vk)
    add_test(NAME NvrhiBackend COMMAND lumin_nvrhi_backend_tests)

    add_executable(lumin_frame_graph_tests tests/FrameGraphTests.cpp src/render/FrameGraph.cpp)
    target_include_directories(lumin_frame_graph_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lumin_frame_graph_tests PRIVATE nvrhi)
    if(WIN32)
        target_compile_definitions(lumin_frame_graph_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME FrameGraphNvRhiManualBarriers COMMAND lumin_frame_graph_tests)

    add_executable(lumin_nvrhi_resource_tests tests/NvrhiResourceTests.cpp)
    target_include_directories(lumin_nvrhi_resource_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lumin_nvrhi_resource_tests PRIVATE nvrhi)
    add_test(NAME NvrhiResource COMMAND lumin_nvrhi_resource_tests)

    add_executable(lumin_nvrhi_pipeline_tests tests/NvrhiPipelineTests.cpp)
    target_include_directories(lumin_nvrhi_pipeline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(lumin_nvrhi_pipeline_tests
        PRIVATE LUMIN_TEST_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}"
    )
    target_link_libraries(lumin_nvrhi_pipeline_tests PRIVATE nvrhi)
    add_dependencies(lumin_nvrhi_pipeline_tests lumin_shaders)
    add_test(NAME NvrhiPipeline COMMAND lumin_nvrhi_pipeline_tests)

    add_executable(lumin_imgui_renderer_tests
        tests/ImGuiRendererTests.cpp
        src/platform/Window.cpp
        src/render/ImGuiLayer.cpp
        src/render/ShaderLibrary.cpp
    )
    target_include_directories(lumin_imgui_renderer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lumin_imgui_renderer_tests PRIVATE Vulkan::Vulkan SDL3::SDL3 imgui::imgui nvrhi)
    target_compile_definitions(lumin_imgui_renderer_tests PRIVATE LUMIN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_compile_definitions(lumin_imgui_renderer_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME ImGuiRenderer COMMAND lumin_imgui_renderer_tests)

    add_executable(lumin_tests tests/CameraLevelTests.cpp)
    target_link_libraries(lumin_tests PRIVATE Lumin::Rendering)
    target_compile_definitions(lumin_tests PRIVATE
        LUMIN_TEST_ASSET_DIR="${LUMIN_ASSET_DIR_CMAKE}"
        LUMIN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME CameraLevel COMMAND lumin_tests)

    add_executable(lumin_game_engine_tests tests/GameEngineIntegrationTests.cpp)
    target_link_libraries(lumin_game_engine_tests PRIVATE Lumin::GameEngine)
    add_test(NAME GameEngineIntegration COMMAND lumin_game_engine_tests)

    add_executable(lumin_level_renderer_tests tests/LevelRendererTests.cpp)
    target_compile_features(lumin_level_renderer_tests PRIVATE cxx_std_20)
    target_compile_definitions(lumin_level_renderer_tests PRIVATE LUMIN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME LevelRendererRecorder COMMAND lumin_level_renderer_tests)

    add_executable(lumin_render_backend_policy_tests tests/RenderBackendPolicyTests.cpp)
    target_compile_features(lumin_render_backend_policy_tests PRIVATE cxx_std_20)
    target_include_directories(lumin_render_backend_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(lumin_render_backend_policy_tests
        PRIVATE LUMIN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME RenderBackendPolicy COMMAND lumin_render_backend_policy_tests)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuminDescriptorIndexing.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuminTextureManagerTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuminModelRendererNvrhiTests.cmake")

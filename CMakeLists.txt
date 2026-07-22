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
find_package(tinyobjloader CONFIG REQUIRED)

set(LUMIN_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(LUMIN_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
set(LUMIN_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(TO_CMAKE_PATH "${LUMIN_SHADER_OUTPUT_DIR}" LUMIN_SHADER_OUTPUT_DIR_CMAKE)
file(TO_CMAKE_PATH "${LUMIN_ASSET_DIR}" LUMIN_ASSET_DIR_CMAKE)

set(LUMIN_SHADER_OUTPUTS)
lumin_add_slang_graphics_shader(
    OUTPUT_VARIABLE LUMIN_SHADER_OUTPUTS
    SOURCE "${LUMIN_SHADER_SOURCE_DIR}/blinn_phong.slang"
    OUTPUT_DIRECTORY "${LUMIN_SHADER_OUTPUT_DIR}"
    MATRIX_LAYOUT column-major
)
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
)

add_custom_target(lumin_shaders ALL
    DEPENDS ${LUMIN_SHADER_OUTPUTS}
)

add_library(lumin_engine STATIC)
add_library(Lumin::Engine ALIAS lumin_engine)

target_sources(lumin_engine
    PRIVATE
        src/assets/ObjLoader.cpp
        src/core/Application.cpp
        src/platform/Window.cpp
        src/scene/Camera.cpp
        src/scene/CameraController.cpp
        src/scene/Level.cpp
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

target_include_directories(lumin_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(lumin_engine PUBLIC cxx_std_20)

target_link_libraries(lumin_engine
    PUBLIC
        Vulkan::Vulkan
        SDL3::SDL3
        glm::glm
        imgui::imgui
        tinyobjloader::tinyobjloader
)

if(WIN32)
    target_compile_definitions(lumin_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_compile_definitions(lumin_engine
    PRIVATE
        $<$<CONFIG:Debug>:LUMIN_ENABLE_VALIDATION>
        LUMIN_ASSET_DIR="${LUMIN_ASSET_DIR_CMAKE}"
        LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}"
)

if(MSVC)
    target_compile_options(lumin_engine PUBLIC /utf-8 PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(lumin_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LUMIN_BUILD_SANDBOX)
    add_executable(lumin_sandbox apps/sandbox/main.cpp)
    target_link_libraries(lumin_sandbox PRIVATE Lumin::Engine)
    add_dependencies(lumin_sandbox lumin_shaders)
    set_target_properties(lumin_sandbox PROPERTIES OUTPUT_NAME LuminEngine)
endif()

if(LUMIN_BUILD_TESTS)
    enable_testing()
    add_executable(lumin_tests tests/CameraLevelTests.cpp)
    target_link_libraries(lumin_tests PRIVATE Lumin::Engine)
    add_test(NAME CameraLevel COMMAND lumin_tests)
endif()

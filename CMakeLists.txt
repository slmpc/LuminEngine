cmake_minimum_required(VERSION 3.25)

set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/cmake/vcpkg-overlays" CACHE STRING
    "Project-local vcpkg overlay ports")

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

# 模块在各自的 CMakeLists.txt 中发现依赖；将 imported targets 提升到目录树全局，供 tests/apps 复用。
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

option(LUMIN_BUILD_SANDBOX "Build the sandbox executable." ON)
option(LUMIN_BUILD_TESTS "Build the engine tests." ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(LuminTargets)
include(LuminRayTracing)

set(LUMIN_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
lumin_configure_ray_tracing_policy(OUTPUT_DIRECTORY "${LUMIN_GENERATED_INCLUDE_DIR}")

set(LUMIN_SHADER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/shaders")
set(LUMIN_SHADER_OUTPUT_DIR "${PROJECT_BINARY_DIR}/generated/shaders")
set(LUMIN_SHADER_MANIFEST "${LUMIN_SHADER_OUTPUT_DIR}/shader-manifest.json" CACHE INTERNAL
    "Generated shader manifest used by ABI validation")
set(LUMIN_ASSET_DIR "${PROJECT_SOURCE_DIR}/assets")
file(TO_CMAKE_PATH "${LUMIN_SHADER_OUTPUT_DIR}" LUMIN_SHADER_OUTPUT_DIR_CMAKE)
file(TO_CMAKE_PATH "${LUMIN_ASSET_DIR}" LUMIN_ASSET_DIR_CMAKE)

add_subdirectory(thirdparty)
add_subdirectory(shaders)
add_subdirectory(core)
add_subdirectory(render)

# Application 是组合层：它可以同时看到 Core 和 Render，但 Render 永远不反向依赖它。
add_library(lumin_application STATIC
    application/Application.cpp
)
add_library(Lumin::Application ALIAS lumin_application)
add_library(Lumin::GameEngine ALIAS lumin_application)
lumin_configure_library(lumin_application)
target_link_libraries(lumin_application
    PUBLIC
        Lumin::Core
        Lumin::Render
)
target_compile_definitions(lumin_application
    PRIVATE
        $<$<CONFIG:Debug>:LUMIN_ENABLE_VALIDATION>
        LUMIN_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}"
)

add_library(lumin_engine INTERFACE)
add_library(Lumin::Engine ALIAS lumin_engine)
target_link_libraries(lumin_engine INTERFACE Lumin::Application)

if(LUMIN_BUILD_SANDBOX)
    add_subdirectory(apps/sandbox)
endif()

if(LUMIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

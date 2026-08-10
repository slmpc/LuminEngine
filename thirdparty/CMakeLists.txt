option(LUMIN_ENABLE_SHARC "Build the SHARC integration boundary." ON)
option(LUMIN_ENABLE_NRD "Build the NRD integration boundary." ON)

# 这两个解析结果是 source、shader 与 test 三层共同使用的唯一 feature gate。
# option 表示用户意图；AVAILABLE 还同时包含当前 RT 构建策略。
set(LUMIN_SHARC_AVAILABLE OFF)
set(LUMIN_NRD_AVAILABLE OFF)

add_library(lumin_sharc INTERFACE)
add_library(Lumin::Sharc ALIAS lumin_sharc)
if(LUMIN_ENABLE_SHARC AND NOT LUMIN_RAY_TRACING STREQUAL "OFF")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sharc/include/SharcCommon.h")
        message(FATAL_ERROR
            "SHARC submodule is missing. Run: git submodule update --init --recursive thirdparty/sharc"
        )
    endif()
    target_include_directories(lumin_sharc SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/sharc/include")
    target_compile_definitions(lumin_sharc INTERFACE LUMIN_HAS_SHARC=1)
    set(LUMIN_SHARC_AVAILABLE ON)
else()
    target_compile_definitions(lumin_sharc INTERFACE LUMIN_HAS_SHARC=0)
endif()

add_library(lumin_nrd INTERFACE)
add_library(Lumin::Nrd ALIAS lumin_nrd)
if(LUMIN_ENABLE_NRD AND NOT LUMIN_RAY_TRACING STREQUAL "OFF")
    foreach(_lumin_nrd_dependency nrd shadermake mathlib)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_lumin_nrd_dependency}/CMakeLists.txt")
            message(FATAL_ERROR
                "NRD dependency '${_lumin_nrd_dependency}' is missing. "
                "Run: git submodule update --init --recursive"
            )
        endif()
    endforeach()

    # 所有传递依赖均来自固定 submodule；禁止上游 CMake 在 configure 阶段下载工具。
    set(SHADERMAKE_FIND_COMPILERS ON CACHE BOOL "" FORCE)
    set(SHADERMAKE_FIND_DXC OFF CACHE BOOL "" FORCE)
    set(SHADERMAKE_FIND_DXC_VK ON CACHE BOOL "" FORCE)
    set(SHADERMAKE_FIND_FXC OFF CACHE BOOL "" FORCE)
    set(SHADERMAKE_FIND_SLANG OFF CACHE BOOL "" FORCE)
    set(SHADERMAKE_TOOL OFF CACHE BOOL "" FORCE)

    set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
    set(NRD_NRI OFF CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_SPIRV_SHADERS ON CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
    set(NRD_SHADERS_PATH "${PROJECT_BINARY_DIR}/generated/nrd" CACHE STRING "" FORCE)

    add_subdirectory(mathlib EXCLUDE_FROM_ALL)
    add_subdirectory(shadermake EXCLUDE_FROM_ALL)
    add_subdirectory(nrd EXCLUDE_FROM_ALL)

    # NRD 4.17.3 将 Clang 一律视作 ELF 工具链并附加 -fPIC。Windows preset 使用
    # clang.exe 生成 MSVC ABI 对象，该驱动不接受此选项，因此仅在该组合下过滤它。
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        get_target_property(_lumin_nrd_compile_options NRD COMPILE_OPTIONS)
        if(_lumin_nrd_compile_options)
            string(REPLACE "-fPIC" "" _lumin_nrd_compile_options "${_lumin_nrd_compile_options}")
            set_property(TARGET NRD PROPERTY COMPILE_OPTIONS "${_lumin_nrd_compile_options}")
        endif()
    endif()

    target_link_libraries(lumin_nrd INTERFACE NRD)
    target_compile_definitions(lumin_nrd INTERFACE LUMIN_HAS_NRD=1)
    set(LUMIN_NRD_AVAILABLE ON)
else()
    target_compile_definitions(lumin_nrd INTERFACE LUMIN_HAS_NRD=0)
endif()

set(LUMIN_SHARC_AVAILABLE "${LUMIN_SHARC_AVAILABLE}" PARENT_SCOPE)
set(LUMIN_NRD_AVAILABLE "${LUMIN_NRD_AVAILABLE}" PARENT_SCOPE)
message(STATUS
    "Lumin vendor rendering features: NRD=${LUMIN_NRD_AVAILABLE}, SHARC=${LUMIN_SHARC_AVAILABLE}"
)

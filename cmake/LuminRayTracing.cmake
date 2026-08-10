include_guard(GLOBAL)

set(_LUMIN_RAY_TRACING_VALUES AUTO ON OFF)

# 策略验证集中在本模块中，主工程和独立 configure 测试执行完全相同的逻辑。
function(lumin_configure_ray_tracing_policy)
    set(_one_value_args OUTPUT_DIRECTORY)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${_one_value_args}" "")

    if(NOT ARG_OUTPUT_DIRECTORY)
        message(FATAL_ERROR "lumin_configure_ray_tracing_policy requires OUTPUT_DIRECTORY.")
    endif()

    set(LUMIN_RAY_TRACING "AUTO" CACHE STRING
        "Build Vulkan ray tracing support: AUTO, ON, or OFF."
    )
    set_property(CACHE LUMIN_RAY_TRACING PROPERTY STRINGS ${_LUMIN_RAY_TRACING_VALUES})

    string(TOUPPER "${LUMIN_RAY_TRACING}" _lumin_ray_tracing_normalized)
    if(NOT _lumin_ray_tracing_normalized IN_LIST _LUMIN_RAY_TRACING_VALUES)
        message(FATAL_ERROR
            "Invalid LUMIN_RAY_TRACING='${LUMIN_RAY_TRACING}'. Expected one of: AUTO, ON, OFF."
        )
    endif()

    # Cache the canonical spelling so generated build metadata and diagnostics are stable.
    set(LUMIN_RAY_TRACING "${_lumin_ray_tracing_normalized}" CACHE STRING
        "Build Vulkan ray tracing support: AUTO, ON, or OFF." FORCE
    )
    set_property(CACHE LUMIN_RAY_TRACING PROPERTY STRINGS ${_LUMIN_RAY_TRACING_VALUES})

    if(LUMIN_RAY_TRACING STREQUAL "OFF")
        set(LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE false)
    else()
        set(LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE true)
    endif()

    if(LUMIN_RAY_TRACING STREQUAL "AUTO")
        set(LUMIN_RAY_TRACING_MODE_ENUM Auto)
    elseif(LUMIN_RAY_TRACING STREQUAL "ON")
        set(LUMIN_RAY_TRACING_MODE_ENUM On)
    else()
        set(LUMIN_RAY_TRACING_MODE_ENUM Off)
    endif()

    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIRECTORY}/render")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RayTracingBuildConfiguration.hpp.in"
        "${ARG_OUTPUT_DIRECTORY}/render/RayTracingBuildConfiguration.hpp"
        @ONLY
    )

    set(LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE
        "${LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE}" PARENT_SCOPE
    )
    set(LUMIN_RAY_TRACING_GENERATED_INCLUDE_DIR "${ARG_OUTPUT_DIRECTORY}" PARENT_SCOPE)
    message(STATUS
        "Lumin ray tracing build policy: ${LUMIN_RAY_TRACING} "
        "(implementation available: ${LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE})"
    )
endfunction()

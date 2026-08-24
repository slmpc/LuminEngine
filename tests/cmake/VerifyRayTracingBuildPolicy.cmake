if(NOT LUMIN_ENGINE_SOURCE_DIR OR NOT LUMIN_TEST_BINARY_DIR OR NOT LUMIN_TEST_GENERATOR)
    message(FATAL_ERROR "Source directory, binary directory, and generator are required.")
endif()

set(_fixture "${LUMIN_ENGINE_SOURCE_DIR}/tests/cmake/RayTracingPolicyFixture")

function(_verify_mode mode enum_value availability)
    set(_binary "${LUMIN_TEST_BINARY_DIR}/${mode}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_fixture}" -B "${_binary}"
                -G "${LUMIN_TEST_GENERATOR}"
                "-DLUMIN_ENGINE_SOURCE_DIR=${LUMIN_ENGINE_SOURCE_DIR}"
                "-DLUMIN_RAY_TRACING=${mode}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${mode} fixture configure failed:\n${_stdout}\n${_stderr}")
    endif()

    file(READ "${_binary}/generated/include/render/RayTracingBuildConfiguration.hpp" _header)
    if(NOT _header MATCHES "RayTracingMode::${enum_value}")
        message(FATAL_ERROR "${mode} generated the wrong RayTracingMode enum value.")
    endif()
    if(NOT _header MATCHES "rayTracingImplementationAvailable = ${availability};")
        message(FATAL_ERROR "${mode} generated the wrong implementation availability.")
    endif()
    if(availability)
        set(_availability_define 1)
    else()
        set(_availability_define 0)
    endif()
    if(NOT _header MATCHES "#define LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE ${_availability_define}")
        message(FATAL_ERROR "${mode} generated the wrong implementation availability macro.")
    endif()
endfunction()

_verify_mode(AUTO Auto true)
_verify_mode(ON On true)
_verify_mode(OFF Off false)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_fixture}" -B "${LUMIN_TEST_BINARY_DIR}/INVALID"
            -G "${LUMIN_TEST_GENERATOR}"
            "-DLUMIN_ENGINE_SOURCE_DIR=${LUMIN_ENGINE_SOURCE_DIR}"
            -DLUMIN_RAY_TRACING=MAYBE
    RESULT_VARIABLE _invalid_result
    OUTPUT_VARIABLE _invalid_stdout
    ERROR_VARIABLE _invalid_stderr
)
if(_invalid_result EQUAL 0)
    message(FATAL_ERROR "An invalid LUMIN_RAY_TRACING value unexpectedly configured successfully.")
endif()
if(NOT "${_invalid_stdout}\n${_invalid_stderr}" MATCHES "Expected one of: AUTO, ON, OFF")
    message(FATAL_ERROR "The invalid-value diagnostic did not list the accepted values.")
endif()

message(STATUS "Ray tracing build policy configure tests passed.")

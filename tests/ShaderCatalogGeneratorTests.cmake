foreach(_required
        LUMIN_SHADER_CATALOG_GENERATOR
        LUMIN_SHADER_SOURCE_DIR
        LUMIN_SHADER_TEST_OUTPUT_DIR
        LUMIN_SLANGC_EXECUTABLE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Shader Catalog generator test requires ${_required}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${LUMIN_SHADER_TEST_OUTPUT_DIR}")
set(_manifest "${LUMIN_SHADER_TEST_OUTPUT_DIR}/shader-manifest.json")
set(_targets "${LUMIN_SHADER_TEST_OUTPUT_DIR}/shader-targets.cmake")
set(_command
    "${LUMIN_SHADER_CATALOG_GENERATOR}"
    --source-dir "${LUMIN_SHADER_SOURCE_DIR}"
    --output-dir "${LUMIN_SHADER_TEST_OUTPUT_DIR}"
    --slangc "${LUMIN_SLANGC_EXECUTABLE}"
    --ray-tracing "${LUMIN_SHADER_ENABLE_RAY_TRACING}"
    --nrd "${LUMIN_SHADER_ENABLE_NRD}"
    --sharc "${LUMIN_SHADER_ENABLE_SHARC}"
)

execute_process(COMMAND ${_command} RESULT_VARIABLE _first_result ERROR_VARIABLE _first_error)
if(NOT _first_result EQUAL 0)
    message(FATAL_ERROR "First Shader Catalog generation failed: ${_first_error}")
endif()
file(TIMESTAMP "${_manifest}" _manifest_before "%s")
file(TIMESTAMP "${_targets}" _targets_before "%s")

# 秒级文件系统时间戳也必须足以观察到意外重写。
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
execute_process(COMMAND ${_command} RESULT_VARIABLE _second_result ERROR_VARIABLE _second_error)
if(NOT _second_result EQUAL 0)
    message(FATAL_ERROR "Second Shader Catalog generation failed: ${_second_error}")
endif()
file(TIMESTAMP "${_manifest}" _manifest_after "%s")
file(TIMESTAMP "${_targets}" _targets_after "%s")

if(NOT _manifest_before STREQUAL _manifest_after OR NOT _targets_before STREQUAL _targets_after)
    message(FATAL_ERROR "Unchanged Shader Catalog generation rewrote its outputs")
endif()

file(READ "${_targets}" _targets_text)
if(NOT _targets_text MATCHES "DEPENDS [^\n]*\\.slang")
    message(FATAL_ERROR "Generated shader commands do not depend on their Slang source")
endif()

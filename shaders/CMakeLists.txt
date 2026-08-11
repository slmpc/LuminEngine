include(LuminShaders)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

file(GLOB_RECURSE _lumin_shader_configs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.json"
)
# Shader sources and includes are build inputs tracked by each slangc depfile. Only
# metadata changes need to regenerate the CMake custom commands.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_lumin_shader_configs}
    "${PROJECT_SOURCE_DIR}/scripts/shader_manifest.py"
)

set(LUMIN_SHADER_MANIFEST "${LUMIN_SHADER_OUTPUT_DIR}/shader-manifest.json")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/shader_manifest.py"
            generate
            --source-dir "${LUMIN_SHADER_SOURCE_DIR}"
            --output-dir "${LUMIN_SHADER_OUTPUT_DIR}"
            --slangc "${LUMIN_SLANGC_EXECUTABLE}"
            --ray-tracing "${LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE}"
            --nrd "${LUMIN_NRD_AVAILABLE}"
            --sharc "${LUMIN_SHARC_AVAILABLE}"
    RESULT_VARIABLE _lumin_shader_generate_result
    OUTPUT_VARIABLE _lumin_shader_generate_stdout
    ERROR_VARIABLE _lumin_shader_generate_stderr
)
if(NOT _lumin_shader_generate_result EQUAL 0)
    message(FATAL_ERROR
        "shader_manifest.py failed with ${_lumin_shader_generate_result}:\n"
        "${_lumin_shader_generate_stdout}${_lumin_shader_generate_stderr}"
    )
endif()

include("${LUMIN_SHADER_OUTPUT_DIR}/shader-targets.cmake")
set(LUMIN_SHADER_ABI_VALIDATOR "${PROJECT_SOURCE_DIR}/cmake/ValidateShaderAbi.cmake")
set(LUMIN_SHADER_ABI_STAMP "${LUMIN_SHADER_OUTPUT_DIR}/shader-abi.validated")

# ABI validation only needs to rerun when its generated inputs or validator change.
add_custom_command(
    OUTPUT "${LUMIN_SHADER_ABI_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
            "-DLUMIN_SHADER_MANIFEST=${LUMIN_SHADER_MANIFEST}"
            "-DLUMIN_SHADER_REFLECTION_DIR=${LUMIN_SHADER_OUTPUT_DIR}"
            "-DLUMIN_SHADER_SOURCE_DIR=${LUMIN_SHADER_SOURCE_DIR}"
            "-DLUMIN_SHADER_ENABLE_RAY_TRACING=${LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE}"
            "-DLUMIN_SHADER_ENABLE_NRD=${LUMIN_NRD_AVAILABLE}"
            "-DLUMIN_SHADER_ENABLE_SHARC=${LUMIN_SHARC_AVAILABLE}"
            -P "${LUMIN_SHADER_ABI_VALIDATOR}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${LUMIN_SHADER_ABI_STAMP}"
    DEPENDS
        ${LUMIN_SHADER_REFLECTIONS}
        "${LUMIN_SHADER_MANIFEST}"
        "${PROJECT_SOURCE_DIR}/scripts/shader_manifest.py"
        "${LUMIN_SHADER_ABI_VALIDATOR}"
    COMMENT "Validating shader reflection ABI"
    VERBATIM
)
add_custom_target(lumin_shader_abi DEPENDS "${LUMIN_SHADER_ABI_STAMP}")

add_custom_target(lumin_shaders ALL DEPENDS ${LUMIN_SHADER_OUTPUTS} ${LUMIN_SHADER_REFLECTIONS})
add_dependencies(lumin_shaders lumin_shader_abi)

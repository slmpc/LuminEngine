include(LuminShaders)

set(_lumin_shader_catalog_sources
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderCatalog.hpp"
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderCatalog.cpp"
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderCatalogGenerator.cpp"
)
set(_lumin_shader_catalog_compile_sources
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderCatalog.cpp"
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderCatalogGenerator.cpp"
)
# Shader sources and includes are build inputs tracked by each slangc depfile. Only
# the typed C++ catalog changes need to regenerate the CMake custom commands.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_lumin_shader_catalog_sources}
)

set(LUMIN_SHADER_MANIFEST "${LUMIN_SHADER_OUTPUT_DIR}/shader-manifest.json")
set(_lumin_shader_catalog_tool_dir "${LUMIN_SHADER_OUTPUT_DIR}/catalog-tool")
set(_lumin_shader_catalog_tool "${_lumin_shader_catalog_tool_dir}/LuminShaderCatalogGenerator${CMAKE_EXECUTABLE_SUFFIX}")
file(MAKE_DIRECTORY "${_lumin_shader_catalog_tool_dir}")

# Catalog generator only depends on the C++ standard library. Building it during configure keeps
# per-entry custom commands visible to CMake without introducing Python or handwritten JSON.
set(_lumin_shader_catalog_compile_definitions)
if(MSVC)
    list(APPEND _lumin_shader_catalog_compile_definitions /utf-8 /W4 /permissive-)
endif()
try_compile(_lumin_shader_catalog_compiled
    "${_lumin_shader_catalog_tool_dir}/build"
    SOURCES ${_lumin_shader_catalog_compile_sources}
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    COMPILE_DEFINITIONS ${_lumin_shader_catalog_compile_definitions}
    COPY_FILE "${_lumin_shader_catalog_tool}"
    OUTPUT_VARIABLE _lumin_shader_catalog_compile_output
)
if(NOT _lumin_shader_catalog_compiled)
    message(FATAL_ERROR
        "Failed to compile the C++ shader catalog generator:\n${_lumin_shader_catalog_compile_output}"
    )
endif()
set(LUMIN_SHADER_CATALOG_GENERATOR "${_lumin_shader_catalog_tool}" CACHE INTERNAL
    "Configure-time C++ shader Catalog generator"
)

execute_process(
    COMMAND "${_lumin_shader_catalog_tool}"
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
        "ShaderCatalogGenerator failed with ${_lumin_shader_generate_result}:\n"
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
        ${_lumin_shader_catalog_sources}
        "${LUMIN_SHADER_ABI_VALIDATOR}"
    COMMENT "Validating shader reflection ABI"
    VERBATIM
)
add_custom_target(lumin_shader_abi DEPENDS "${LUMIN_SHADER_ABI_STAMP}")

add_custom_target(lumin_shaders ALL DEPENDS ${LUMIN_SHADER_OUTPUTS} ${LUMIN_SHADER_REFLECTIONS})
add_dependencies(lumin_shaders lumin_shader_abi)

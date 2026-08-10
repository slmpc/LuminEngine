include_guard(GLOBAL)

find_program(LUMIN_SLANGC_EXECUTABLE
    NAMES slangc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/Bin32"
    DOC "Path to the Slang shader compiler"
)

if(NOT LUMIN_SLANGC_EXECUTABLE)
    message(FATAL_ERROR "slangc was not found. Install the Vulkan SDK with Slang support or add slangc to PATH.")
endif()

mark_as_advanced(LUMIN_SLANGC_EXECUTABLE)

function(_lumin_shader_json_get output_variable json_variable)
    string(JSON _value ERROR_VARIABLE _error GET "${${json_variable}}" ${ARGN})
    if(NOT _error STREQUAL "NOTFOUND")
        string(JOIN "." _path ${ARGN})
        message(FATAL_ERROR "Shader manifest is missing or has an invalid '${_path}' value: ${_error}")
    endif()
    set(${output_variable} "${_value}" PARENT_SCOPE)
endfunction()

function(_lumin_shader_json_length output_variable json_variable)
    string(JSON _value ERROR_VARIABLE _error LENGTH "${${json_variable}}" ${ARGN})
    if(NOT _error STREQUAL "NOTFOUND")
        string(JOIN "." _path ${ARGN})
        message(FATAL_ERROR "Shader manifest is missing or has an invalid '${_path}' array: ${_error}")
    endif()
    set(${output_variable} "${_value}" PARENT_SCOPE)
endfunction()

function(lumin_add_slang_shader)
    set(_one_value_args
        OUTPUT_VARIABLE
        REFLECTION_VARIABLE
        SOURCE
        OUTPUT
        REFLECTION
        DEPFILE
        ENTRY_POINT
        STAGE
        TARGET
        PROFILE
        MATRIX_LAYOUT
        WARNINGS_AS_ERRORS
    )
    set(_multi_value_args CAPABILITIES DEFINES DEPENDS INCLUDE_DIRECTORIES OPTIONS)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${_one_value_args}" "${_multi_value_args}")

    foreach(_required_arg OUTPUT_VARIABLE REFLECTION_VARIABLE SOURCE OUTPUT REFLECTION DEPFILE ENTRY_POINT STAGE)
        if(NOT ARG_${_required_arg})
            message(FATAL_ERROR "lumin_add_slang_shader requires ${_required_arg}.")
        endif()
    endforeach()

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "lumin_add_slang_shader received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT ARG_TARGET)
        set(ARG_TARGET spirv)
    endif()
    if(NOT ARG_PROFILE)
        set(ARG_PROFILE spirv_1_5)
    endif()
    if(NOT ARG_WARNINGS_AS_ERRORS)
        set(ARG_WARNINGS_AS_ERRORS all)
    endif()

    get_filename_component(_source "${ARG_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_output "${ARG_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(_reflection "${ARG_REFLECTION}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(_depfile "${ARG_DEPFILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(_output_directory "${_output}" DIRECTORY)
    get_filename_component(_reflection_directory "${_reflection}" DIRECTORY)
    get_filename_component(_depfile_directory "${_depfile}" DIRECTORY)

    set(_compiler_options
        -target "${ARG_TARGET}"
        -profile "${ARG_PROFILE}"
        -warnings-as-errors "${ARG_WARNINGS_AS_ERRORS}"
    )
    if(ARG_TARGET STREQUAL "spirv")
        list(APPEND _compiler_options -fvk-use-entrypoint-name)
    endif()
    if(ARG_MATRIX_LAYOUT)
        list(APPEND _compiler_options "-matrix-layout-${ARG_MATRIX_LAYOUT}")
    endif()
    if(ARG_CAPABILITIES)
        list(JOIN ARG_CAPABILITIES "+" _capability_expression)
        list(APPEND _compiler_options -capability "${_capability_expression}")
    endif()
    foreach(_include_directory IN LISTS ARG_INCLUDE_DIRECTORIES)
        list(APPEND _compiler_options -I "${_include_directory}")
    endforeach()
    foreach(_define IN LISTS ARG_DEFINES)
        list(APPEND _compiler_options -D "${_define}")
    endforeach()
    list(APPEND _compiler_options
        -entry "${ARG_ENTRY_POINT}"
        -stage "${ARG_STAGE}"
        -reflection-json "${_reflection}"
        -depfile "${_depfile}"
        ${ARG_OPTIONS}
    )

    add_custom_command(
        OUTPUT "${_output}" "${_reflection}"
        BYPRODUCTS "${_depfile}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${_output_directory}" "${_reflection_directory}" "${_depfile_directory}"
        COMMAND "${LUMIN_SLANGC_EXECUTABLE}" "${_source}" ${_compiler_options} -o "${_output}"
        DEPENDS "${_source}" ${ARG_DEPENDS}
        DEPFILE "${_depfile}"
        COMMENT "Compiling ${ARG_STAGE} shader ${_source} with reflection"
        VERBATIM
        COMMAND_EXPAND_LISTS
    )

    set(_outputs "${${ARG_OUTPUT_VARIABLE}}")
    list(APPEND _outputs "${_output}")
    set(${ARG_OUTPUT_VARIABLE} "${_outputs}" PARENT_SCOPE)

    set(_reflections "${${ARG_REFLECTION_VARIABLE}}")
    list(APPEND _reflections "${_reflection}")
    set(${ARG_REFLECTION_VARIABLE} "${_reflections}" PARENT_SCOPE)
endfunction()

function(_lumin_shader_feature_enabled output_variable feature)
    if(feature STREQUAL "rayTracing")
        set(_enabled "${LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE}")
    elseif(feature STREQUAL "nrd")
        set(_enabled "${LUMIN_NRD_AVAILABLE}")
    elseif(feature STREQUAL "sharc")
        set(_enabled "${LUMIN_SHARC_AVAILABLE}")
    else()
        message(FATAL_ERROR "Shader manifest uses unknown feature requirement '${feature}'.")
    endif()
    if(_enabled)
        set(${output_variable} TRUE PARENT_SCOPE)
    else()
        set(${output_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_lumin_shader_manifest_entry_enabled output_variable manifest_variable shader_index)
    set(_enabled TRUE)
    string(JSON _requirement_count ERROR_VARIABLE _requirement_error
           LENGTH "${${manifest_variable}}" shaders ${shader_index} requires)
    if(_requirement_error STREQUAL "NOTFOUND" AND _requirement_count GREATER 0)
        math(EXPR _last_requirement "${_requirement_count} - 1")
        foreach(_requirement_index RANGE 0 ${_last_requirement})
            _lumin_shader_json_get(_requirement ${manifest_variable}
                                   shaders ${shader_index} requires ${_requirement_index})
            _lumin_shader_feature_enabled(_requirement_enabled "${_requirement}")
            if(NOT _requirement_enabled)
                set(_enabled FALSE)
            endif()
        endforeach()
    elseif(NOT _requirement_error STREQUAL "NOTFOUND")
        set(_enabled TRUE)
    endif()
    set(${output_variable} "${_enabled}" PARENT_SCOPE)
endfunction()

function(lumin_add_slang_manifest)
    set(_one_value_args
        MANIFEST
        SOURCE_DIRECTORY
        OUTPUT_DIRECTORY
        OUTPUT_VARIABLE
        REFLECTION_VARIABLE
    )
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${_one_value_args}" "")

    foreach(_required_arg MANIFEST SOURCE_DIRECTORY OUTPUT_DIRECTORY OUTPUT_VARIABLE REFLECTION_VARIABLE)
        if(NOT ARG_${_required_arg})
            message(FATAL_ERROR "lumin_add_slang_manifest requires ${_required_arg}.")
        endif()
    endforeach()

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "lumin_add_slang_manifest received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    get_filename_component(_manifest "${ARG_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_source_directory "${ARG_SOURCE_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_output_directory "${ARG_OUTPUT_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest}")
    file(READ "${_manifest}" _manifest_json)

    _lumin_shader_json_get(_schema_version _manifest_json schemaVersion)
    if(NOT _schema_version EQUAL 1)
        message(FATAL_ERROR "Unsupported shader manifest schema version: ${_schema_version}")
    endif()

    _lumin_shader_json_get(_target _manifest_json compiler target)
    _lumin_shader_json_get(_profile _manifest_json compiler profile)
    _lumin_shader_json_get(_matrix_layout _manifest_json compiler matrixLayout)
    _lumin_shader_json_get(_warnings_as_errors _manifest_json compiler warningsAsErrors)
    if(NOT _warnings_as_errors STREQUAL "all")
        message(FATAL_ERROR "Shader manifest compiler.warningsAsErrors must be 'all'.")
    endif()

    set(_include_directories)
    _lumin_shader_json_length(_include_count _manifest_json compiler includeDirectories)
    if(_include_count GREATER 0)
        math(EXPR _last_include "${_include_count} - 1")
        foreach(_include_index RANGE 0 ${_last_include})
            _lumin_shader_json_get(_include_path _manifest_json compiler includeDirectories ${_include_index})
            get_filename_component(_include_path "${_include_path}" ABSOLUTE BASE_DIR "${_source_directory}")
            if(NOT IS_DIRECTORY "${_include_path}")
                message(FATAL_ERROR "Shader include directory does not exist: ${_include_path}")
            endif()
            list(APPEND _include_directories "${_include_path}")
        endforeach()
    endif()

    set(_outputs "${${ARG_OUTPUT_VARIABLE}}")
    set(_reflections "${${ARG_REFLECTION_VARIABLE}}")
    set(_registered_outputs)
    _lumin_shader_json_length(_shader_count _manifest_json shaders)
    if(_shader_count LESS 1)
        message(FATAL_ERROR "Shader manifest must contain at least one shader entry.")
    endif()

    math(EXPR _last_shader "${_shader_count} - 1")
    foreach(_shader_index RANGE 0 ${_last_shader})
        _lumin_shader_json_get(_name _manifest_json shaders ${_shader_index} name)
        _lumin_shader_json_get(_source _manifest_json shaders ${_shader_index} source)
        _lumin_shader_json_get(_entry _manifest_json shaders ${_shader_index} entry)
        _lumin_shader_json_get(_stage _manifest_json shaders ${_shader_index} stage)
        _lumin_shader_json_get(_output _manifest_json shaders ${_shader_index} output)
        _lumin_shader_json_get(_reflection _manifest_json shaders ${_shader_index} reflection)
        _lumin_shader_json_get(_depfile _manifest_json shaders ${_shader_index} depfile)

        _lumin_shader_manifest_entry_enabled(_entry_enabled _manifest_json ${_shader_index})
        if(NOT _entry_enabled)
            message(STATUS "Skipping shader '${_name}' because a required rendering feature is disabled.")
            continue()
        endif()

        if(_output IN_LIST _registered_outputs)
            message(FATAL_ERROR "Shader manifest output is registered more than once: ${_output}")
        endif()
        list(APPEND _registered_outputs "${_output}")

        set(_capabilities)
        string(JSON _capability_count ERROR_VARIABLE _capability_error
               LENGTH "${_manifest_json}" shaders ${_shader_index} capabilities)
        if(_capability_error STREQUAL "NOTFOUND" AND _capability_count GREATER 0)
            math(EXPR _last_capability "${_capability_count} - 1")
            foreach(_capability_index RANGE 0 ${_last_capability})
                _lumin_shader_json_get(_capability _manifest_json shaders ${_shader_index}
                                       capabilities ${_capability_index})
                list(APPEND _capabilities "${_capability}")
            endforeach()
        elseif(NOT _capability_error STREQUAL "NOTFOUND")
            set(_capabilities)
        endif()

        set(_entry_include_directories ${_include_directories})
        string(JSON _entry_include_count ERROR_VARIABLE _entry_include_error
               LENGTH "${_manifest_json}" shaders ${_shader_index} includeDirectories)
        if(_entry_include_error STREQUAL "NOTFOUND" AND _entry_include_count GREATER 0)
            math(EXPR _last_entry_include "${_entry_include_count} - 1")
            foreach(_entry_include_index RANGE 0 ${_last_entry_include})
                _lumin_shader_json_get(_entry_include _manifest_json shaders ${_shader_index}
                                       includeDirectories ${_entry_include_index})
                get_filename_component(_entry_include "${_entry_include}" ABSOLUTE
                                       BASE_DIR "${_source_directory}")
                if(NOT IS_DIRECTORY "${_entry_include}")
                    message(FATAL_ERROR "Shader include directory does not exist: ${_entry_include}")
                endif()
                list(APPEND _entry_include_directories "${_entry_include}")
            endforeach()
        endif()

        set(_defines)
        string(JSON _define_count ERROR_VARIABLE _define_error
               LENGTH "${_manifest_json}" shaders ${_shader_index} defines)
        if(_define_error STREQUAL "NOTFOUND" AND _define_count GREATER 0)
            math(EXPR _last_define "${_define_count} - 1")
            foreach(_define_index RANGE 0 ${_last_define})
                _lumin_shader_json_get(_define _manifest_json shaders ${_shader_index} defines ${_define_index})
                list(APPEND _defines "${_define}")
            endforeach()
        endif()

        lumin_add_slang_shader(
            OUTPUT_VARIABLE _outputs
            REFLECTION_VARIABLE _reflections
            SOURCE "${_source_directory}/${_source}"
            OUTPUT "${_output_directory}/${_output}"
            REFLECTION "${_output_directory}/${_reflection}"
            DEPFILE "${_output_directory}/${_depfile}"
            ENTRY_POINT "${_entry}"
            STAGE "${_stage}"
            TARGET "${_target}"
            PROFILE "${_profile}"
            MATRIX_LAYOUT "${_matrix_layout}"
            WARNINGS_AS_ERRORS "${_warnings_as_errors}"
            CAPABILITIES ${_capabilities}
            DEFINES ${_defines}
            INCLUDE_DIRECTORIES ${_entry_include_directories}
            DEPENDS "${_manifest}"
        )
    endforeach()

    set(${ARG_OUTPUT_VARIABLE} "${_outputs}" PARENT_SCOPE)
    set(${ARG_REFLECTION_VARIABLE} "${_reflections}" PARENT_SCOPE)
endfunction()

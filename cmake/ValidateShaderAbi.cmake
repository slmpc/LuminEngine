cmake_minimum_required(VERSION 3.25)

foreach(_required_variable LUMIN_SHADER_MANIFEST LUMIN_SHADER_REFLECTION_DIR LUMIN_SHADER_SOURCE_DIR)
    if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "ValidateShaderAbi.cmake requires ${_required_variable}.")
    endif()
endforeach()

function(_lumin_abi_json_get output_variable json_variable)
    string(JSON _value ERROR_VARIABLE _error GET "${${json_variable}}" ${ARGN})
    if(NOT _error STREQUAL "NOTFOUND")
        string(JOIN "." _path ${ARGN})
        message(FATAL_ERROR "Shader ABI JSON is missing or has an invalid '${_path}' value: ${_error}")
    endif()
    set(${output_variable} "${_value}" PARENT_SCOPE)
endfunction()

function(_lumin_abi_json_length output_variable json_variable)
    string(JSON _value ERROR_VARIABLE _error LENGTH "${${json_variable}}" ${ARGN})
    if(NOT _error STREQUAL "NOTFOUND")
        string(JOIN "." _path ${ARGN})
        message(FATAL_ERROR "Shader ABI JSON is missing or has an invalid '${_path}' array: ${_error}")
    endif()
    set(${output_variable} "${_value}" PARENT_SCOPE)
endfunction()

function(_lumin_abi_json_optional_get output_variable found_variable json_variable)
    string(JSON _value ERROR_VARIABLE _error GET "${${json_variable}}" ${ARGN})
    if(_error STREQUAL "NOTFOUND")
        set(${output_variable} "${_value}" PARENT_SCOPE)
        set(${found_variable} TRUE PARENT_SCOPE)
    else()
        set(${output_variable} "" PARENT_SCOPE)
        set(${found_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

foreach(_feature_variable
        LUMIN_SHADER_ENABLE_RAY_TRACING
        LUMIN_SHADER_ENABLE_NRD
        LUMIN_SHADER_ENABLE_SHARC)
    if(NOT DEFINED ${_feature_variable})
        set(${_feature_variable} ON)
    endif()
endforeach()

function(_lumin_abi_feature_enabled output_variable feature)
    if(feature STREQUAL "rayTracing")
        set(_enabled "${LUMIN_SHADER_ENABLE_RAY_TRACING}")
    elseif(feature STREQUAL "nrd")
        set(_enabled "${LUMIN_SHADER_ENABLE_NRD}")
    elseif(feature STREQUAL "sharc")
        set(_enabled "${LUMIN_SHADER_ENABLE_SHARC}")
    else()
        message(FATAL_ERROR "Shader manifest uses unknown feature requirement '${feature}'.")
    endif()
    if(_enabled)
        set(${output_variable} TRUE PARENT_SCOPE)
    else()
        set(${output_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_lumin_abi_shader_enabled output_variable manifest_variable shader_index)
    set(_enabled TRUE)
    string(JSON _requirement_count ERROR_VARIABLE _requirement_error
           LENGTH "${${manifest_variable}}" shaders ${shader_index} requires)
    if(_requirement_error STREQUAL "NOTFOUND" AND _requirement_count GREATER 0)
        math(EXPR _last_requirement "${_requirement_count} - 1")
        foreach(_requirement_index RANGE 0 ${_last_requirement})
            _lumin_abi_json_get(_requirement ${manifest_variable}
                                shaders ${shader_index} requires ${_requirement_index})
            _lumin_abi_feature_enabled(_requirement_enabled "${_requirement}")
            if(NOT _requirement_enabled)
                set(_enabled FALSE)
            endif()
        endforeach()
    elseif(NOT _requirement_error STREQUAL "NOTFOUND")
        set(_enabled TRUE)
    endif()
    set(${output_variable} "${_enabled}" PARENT_SCOPE)
endfunction()

function(_lumin_validate_shader_parameter_kind shader_name reflection_variable parameter_index expected_kind)
    _lumin_abi_json_get(_binding_kind ${reflection_variable}
                        parameters ${parameter_index} binding kind)
    _lumin_abi_json_get(_type_kind ${reflection_variable}
                        parameters ${parameter_index} type kind)

    if(expected_kind STREQUAL "pushConstant")
        if(NOT _binding_kind STREQUAL "pushConstantBuffer" OR NOT _type_kind STREQUAL "constantBuffer")
            message(FATAL_ERROR
                "${shader_name}: expected pushConstant, got binding=${_binding_kind}, type=${_type_kind}.")
        endif()
        return()
    endif()

    if(NOT _binding_kind STREQUAL "descriptorTableSlot")
        message(FATAL_ERROR
            "${shader_name}: expected a descriptor binding, got reflection binding kind '${_binding_kind}'.")
    endif()

    if(expected_kind STREQUAL "constantBuffer")
        if(NOT _type_kind STREQUAL "constantBuffer")
            message(FATAL_ERROR "${shader_name}: expected constantBuffer, got '${_type_kind}'.")
        endif()
    elseif(expected_kind STREQUAL "structuredBuffer")
        if(_type_kind STREQUAL "array")
            _lumin_abi_json_get(_resource_kind ${reflection_variable}
                                parameters ${parameter_index} type elementType kind)
            _lumin_abi_json_get(_base_shape ${reflection_variable}
                                parameters ${parameter_index} type elementType baseShape)
        else()
            set(_resource_kind "${_type_kind}")
            _lumin_abi_json_get(_base_shape ${reflection_variable}
                                parameters ${parameter_index} type baseShape)
        endif()
        if(NOT _resource_kind STREQUAL "resource" OR NOT _base_shape STREQUAL "structuredBuffer")
            message(FATAL_ERROR
                "${shader_name}: expected structuredBuffer, got type=${_resource_kind}, shape=${_base_shape}.")
        endif()
    elseif(expected_kind STREQUAL "sampledImage" OR expected_kind STREQUAL "sampledImage3D")
        if(_type_kind STREQUAL "array")
            _lumin_abi_json_get(_resource_kind ${reflection_variable}
                                parameters ${parameter_index} type elementType kind)
            _lumin_abi_json_get(_base_shape ${reflection_variable}
                                parameters ${parameter_index} type elementType baseShape)
        else()
            set(_resource_kind "${_type_kind}")
            _lumin_abi_json_get(_base_shape ${reflection_variable}
                                parameters ${parameter_index} type baseShape)
        endif()
        if(expected_kind STREQUAL "sampledImage3D")
            set(_expected_shape "texture3D")
        else()
            set(_expected_shape "texture2D")
        endif()
        if(NOT _resource_kind STREQUAL "resource" OR NOT _base_shape STREQUAL _expected_shape)
            message(FATAL_ERROR
                "${shader_name}: expected ${expected_kind} ${_expected_shape}, got "
                "type=${_resource_kind}, shape=${_base_shape}.")
        endif()
    elseif(expected_kind STREQUAL "sampler")
        if(NOT _type_kind STREQUAL "samplerState")
            message(FATAL_ERROR "${shader_name}: expected sampler, got '${_type_kind}'.")
        endif()
    elseif(expected_kind STREQUAL "storageImage" OR expected_kind STREQUAL "storageImage3D")
        _lumin_abi_json_get(_base_shape ${reflection_variable}
                            parameters ${parameter_index} type baseShape)
        _lumin_abi_json_get(_access ${reflection_variable}
                            parameters ${parameter_index} type access)
        if(expected_kind STREQUAL "storageImage3D")
            set(_expected_shape "texture3D")
        else()
            set(_expected_shape "texture2D")
        endif()
        if(NOT _type_kind STREQUAL "resource" OR NOT _base_shape STREQUAL _expected_shape OR
           NOT _access STREQUAL "readWrite")
            message(FATAL_ERROR
                "${shader_name}: expected read-write ${expected_kind} ${_expected_shape}, got "
                "type=${_type_kind}, shape=${_base_shape}, access=${_access}.")
        endif()
    elseif(expected_kind STREQUAL "accelerationStructure")
        _lumin_abi_json_get(_base_shape ${reflection_variable}
                            parameters ${parameter_index} type baseShape)
        if(NOT _type_kind STREQUAL "resource" OR NOT _base_shape STREQUAL "accelerationStructure")
            message(FATAL_ERROR
                "${shader_name}: expected accelerationStructure, got type=${_type_kind}, shape=${_base_shape}.")
        endif()
    else()
        message(FATAL_ERROR "${shader_name}: unknown manifest binding kind '${expected_kind}'.")
    endif()
endfunction()

function(_lumin_find_manifest_struct output_variable manifest_variable struct_name)
    _lumin_abi_json_length(_struct_count ${manifest_variable} abi structs)
    set(_found_index -1)
    if(_struct_count GREATER 0)
        math(EXPR _last_struct "${_struct_count} - 1")
        foreach(_struct_index RANGE 0 ${_last_struct})
            _lumin_abi_json_get(_candidate_name ${manifest_variable} abi structs ${_struct_index} name)
            if(_candidate_name STREQUAL struct_name)
                set(_found_index ${_struct_index})
                break()
            endif()
        endforeach()
    endif()
    set(${output_variable} ${_found_index} PARENT_SCOPE)
endfunction()

function(_lumin_validate_shader_struct shader_name reflection_variable manifest_variable struct_name)
    _lumin_find_manifest_struct(_expected_struct_index ${manifest_variable} "${struct_name}")
    if(_expected_struct_index LESS 0)
        message(FATAL_ERROR "${shader_name}: ABI struct '${struct_name}' is not defined in the manifest.")
    endif()

    _lumin_abi_json_length(_parameter_count ${reflection_variable} parameters)
    set(_actual_parameter_index -1)
    set(_actual_field_path)
    set(_actual_size "")
    if(_parameter_count GREATER 0)
        math(EXPR _last_parameter "${_parameter_count} - 1")
        foreach(_parameter_index RANGE 0 ${_last_parameter})
            _lumin_abi_json_get(_parameter_type ${reflection_variable}
                                parameters ${_parameter_index} type kind)
            if(_parameter_type STREQUAL "constantBuffer")
                _lumin_abi_json_optional_get(_candidate_name _has_candidate_name ${reflection_variable}
                    parameters ${_parameter_index} type elementType name)
                if(_has_candidate_name AND _candidate_name STREQUAL struct_name)
                    set(_actual_parameter_index ${_parameter_index})
                    set(_actual_field_path parameters ${_parameter_index} type elementType fields)
                    _lumin_abi_json_get(_actual_size ${reflection_variable}
                        parameters ${_parameter_index} type elementVarLayout binding size)
                    break()
                endif()
            elseif(_parameter_type STREQUAL "resource")
                _lumin_abi_json_optional_get(_candidate_name _has_candidate_name ${reflection_variable}
                    parameters ${_parameter_index} type resultType name)
                if(_has_candidate_name AND _candidate_name STREQUAL struct_name)
                    set(_actual_parameter_index ${_parameter_index})
                    set(_actual_field_path parameters ${_parameter_index} type resultType fields)
                    break()
                endif()
            elseif(_parameter_type STREQUAL "array")
                _lumin_abi_json_optional_get(_candidate_name _has_candidate_name ${reflection_variable}
                    parameters ${_parameter_index} type elementType resultType name)
                if(_has_candidate_name AND _candidate_name STREQUAL struct_name)
                    set(_actual_parameter_index ${_parameter_index})
                    set(_actual_field_path parameters ${_parameter_index} type elementType resultType fields)
                    break()
                endif()
            endif()
        endforeach()
    endif()

    if(_actual_parameter_index LESS 0)
        message(FATAL_ERROR "${shader_name}: reflection does not expose ABI struct '${struct_name}'.")
    endif()

    _lumin_abi_json_get(_expected_size ${manifest_variable}
                        abi structs ${_expected_struct_index} size)
    _lumin_abi_json_length(_expected_field_count ${manifest_variable}
                           abi structs ${_expected_struct_index} fields)
    _lumin_abi_json_length(_actual_field_count ${reflection_variable} ${_actual_field_path})
    if(NOT _actual_field_count EQUAL _expected_field_count)
        message(FATAL_ERROR
            "${shader_name}: ${struct_name} field count changed: expected ${_expected_field_count}, "
            "reflected ${_actual_field_count}.")
    endif()

    set(_computed_size 0)
    if(_expected_field_count GREATER 0)
        math(EXPR _last_field "${_expected_field_count} - 1")
        foreach(_field_index RANGE 0 ${_last_field})
            _lumin_abi_json_get(_expected_name ${manifest_variable}
                abi structs ${_expected_struct_index} fields ${_field_index} name)
            _lumin_abi_json_get(_expected_offset ${manifest_variable}
                abi structs ${_expected_struct_index} fields ${_field_index} offset)
            _lumin_abi_json_get(_expected_field_size ${manifest_variable}
                abi structs ${_expected_struct_index} fields ${_field_index} size)
            _lumin_abi_json_get(_actual_name ${reflection_variable}
                ${_actual_field_path} ${_field_index} name)
            _lumin_abi_json_get(_actual_offset ${reflection_variable}
                ${_actual_field_path} ${_field_index} binding offset)
            _lumin_abi_json_get(_actual_field_size ${reflection_variable}
                ${_actual_field_path} ${_field_index} binding size)

            if(NOT _actual_name STREQUAL _expected_name OR
               NOT _actual_offset EQUAL _expected_offset OR
               NOT _actual_field_size EQUAL _expected_field_size)
                message(FATAL_ERROR
                    "${shader_name}: ${struct_name}.${_expected_name} ABI changed; expected "
                    "offset=${_expected_offset}, size=${_expected_field_size}, reflected "
                    "name=${_actual_name}, offset=${_actual_offset}, size=${_actual_field_size}.")
            endif()

            math(EXPR _field_end "${_actual_offset} + ${_actual_field_size}")
            if(_field_end GREATER _computed_size)
                set(_computed_size ${_field_end})
            endif()
        endforeach()
    endif()

    if(_actual_size STREQUAL "")
        set(_actual_size ${_computed_size})
    endif()
    if(NOT _actual_size EQUAL _expected_size)
        message(FATAL_ERROR
            "${shader_name}: ${struct_name} size changed; expected ${_expected_size}, reflected ${_actual_size}.")
    endif()
endfunction()

get_filename_component(LUMIN_SHADER_MANIFEST "${LUMIN_SHADER_MANIFEST}" ABSOLUTE)
get_filename_component(LUMIN_SHADER_REFLECTION_DIR "${LUMIN_SHADER_REFLECTION_DIR}" ABSOLUTE)
get_filename_component(LUMIN_SHADER_SOURCE_DIR "${LUMIN_SHADER_SOURCE_DIR}" ABSOLUTE)
file(READ "${LUMIN_SHADER_MANIFEST}" _manifest_json)

_lumin_abi_json_get(_schema_version _manifest_json schemaVersion)
if(NOT _schema_version EQUAL 1)
    message(FATAL_ERROR "Unsupported shader manifest schema version: ${_schema_version}")
endif()

_lumin_abi_json_length(_shader_count _manifest_json shaders)
if(_shader_count LESS 1)
    message(FATAL_ERROR "Shader manifest must contain at least one shader entry.")
endif()

set(_shader_names)
set(_shader_outputs)
set(_validated_shader_count 0)
math(EXPR _last_shader "${_shader_count} - 1")
foreach(_shader_index RANGE 0 ${_last_shader})
    _lumin_abi_json_get(_shader_name _manifest_json shaders ${_shader_index} name)
    _lumin_abi_json_get(_shader_source _manifest_json shaders ${_shader_index} source)
    _lumin_abi_json_get(_expected_entry _manifest_json shaders ${_shader_index} entry)
    _lumin_abi_json_get(_expected_stage _manifest_json shaders ${_shader_index} stage)
    _lumin_abi_json_get(_shader_output _manifest_json shaders ${_shader_index} output)
    _lumin_abi_json_get(_reflection_path _manifest_json shaders ${_shader_index} reflection)
    _lumin_abi_json_get(_depfile_path _manifest_json shaders ${_shader_index} depfile)

    if(_shader_name IN_LIST _shader_names)
        message(FATAL_ERROR "Shader manifest name is duplicated: ${_shader_name}")
    endif()
    if(_shader_output IN_LIST _shader_outputs)
        message(FATAL_ERROR "Shader manifest output is duplicated: ${_shader_output}")
    endif()
    list(APPEND _shader_names "${_shader_name}")
    list(APPEND _shader_outputs "${_shader_output}")

    _lumin_abi_shader_enabled(_shader_enabled _manifest_json ${_shader_index})
    if(NOT _shader_enabled)
        continue()
    endif()
    math(EXPR _validated_shader_count "${_validated_shader_count} + 1")

    set(_source_path "${LUMIN_SHADER_SOURCE_DIR}/${_shader_source}")
    set(_reflection_path "${LUMIN_SHADER_REFLECTION_DIR}/${_reflection_path}")
    set(_depfile_path "${LUMIN_SHADER_REFLECTION_DIR}/${_depfile_path}")
    foreach(_required_file "${_source_path}" "${_reflection_path}" "${_depfile_path}")
        if(NOT EXISTS "${_required_file}")
            message(FATAL_ERROR "${_shader_name}: required shader artifact does not exist: ${_required_file}")
        endif()
    endforeach()

    file(READ "${_reflection_path}" _reflection_json)
    _lumin_abi_json_length(_entry_count _reflection_json entryPoints)
    if(NOT _entry_count EQUAL 1)
        message(FATAL_ERROR "${_shader_name}: reflection must contain exactly one entry point.")
    endif()
    _lumin_abi_json_get(_actual_entry _reflection_json entryPoints 0 name)
    _lumin_abi_json_optional_get(_actual_stage _has_actual_stage _reflection_json entryPoints 0 stage)
    set(_ray_tracing_stages raygeneration intersection anyhit closesthit miss callable)
    if(NOT _actual_entry STREQUAL _expected_entry OR
       (_has_actual_stage AND NOT _actual_stage STREQUAL _expected_stage) OR
       (NOT _has_actual_stage AND NOT _expected_stage IN_LIST _ray_tracing_stages))
        message(FATAL_ERROR
            "${_shader_name}: expected entry ${_expected_entry}/${_expected_stage}, "
            "reflected ${_actual_entry}/${_actual_stage}.")
    endif()

    _lumin_abi_json_length(_expected_binding_count _manifest_json shaders ${_shader_index} bindings)
    _lumin_abi_json_length(_actual_parameter_count _reflection_json parameters)
    if(NOT _actual_parameter_count EQUAL _expected_binding_count)
        message(FATAL_ERROR
            "${_shader_name}: global binding count changed; expected ${_expected_binding_count}, "
            "reflected ${_actual_parameter_count}.")
    endif()

    set(_expected_binding_names)
    if(_expected_binding_count GREATER 0)
        math(EXPR _last_binding "${_expected_binding_count} - 1")
        foreach(_binding_index RANGE 0 ${_last_binding})
            _lumin_abi_json_get(_expected_name _manifest_json
                shaders ${_shader_index} bindings ${_binding_index} name)
            _lumin_abi_json_get(_expected_kind _manifest_json
                shaders ${_shader_index} bindings ${_binding_index} kind)
            _lumin_abi_json_get(_expected_slot _manifest_json
                shaders ${_shader_index} bindings ${_binding_index} binding)
            if(_expected_name IN_LIST _expected_binding_names)
                message(FATAL_ERROR "${_shader_name}: binding name is duplicated in manifest: ${_expected_name}")
            endif()
            list(APPEND _expected_binding_names "${_expected_name}")

            set(_actual_parameter_index -1)
            math(EXPR _last_parameter "${_actual_parameter_count} - 1")
            foreach(_parameter_index RANGE 0 ${_last_parameter})
                _lumin_abi_json_get(_actual_name _reflection_json parameters ${_parameter_index} name)
                if(_actual_name STREQUAL _expected_name)
                    set(_actual_parameter_index ${_parameter_index})
                    break()
                endif()
            endforeach()
            if(_actual_parameter_index LESS 0)
                message(FATAL_ERROR "${_shader_name}: expected binding '${_expected_name}' is missing from reflection.")
            endif()

            _lumin_abi_json_get(_actual_slot _reflection_json
                                parameters ${_actual_parameter_index} binding index)
            if(NOT _actual_slot EQUAL _expected_slot)
                message(FATAL_ERROR
                    "${_shader_name}: binding '${_expected_name}' moved from ${_expected_slot} to ${_actual_slot}.")
            endif()
            _lumin_validate_shader_parameter_kind(
                "${_shader_name}.${_expected_name}" _reflection_json ${_actual_parameter_index} "${_expected_kind}")

            if(NOT _expected_kind STREQUAL "pushConstant")
                _lumin_abi_json_get(_expected_set _manifest_json
                    shaders ${_shader_index} bindings ${_binding_index} set)
                _lumin_abi_json_optional_get(_actual_set _has_actual_set _reflection_json
                    parameters ${_actual_parameter_index} binding space)
                if(NOT _has_actual_set)
                    set(_actual_set 0)
                endif()
                if(NOT _actual_set EQUAL _expected_set)
                    message(FATAL_ERROR
                        "${_shader_name}: binding '${_expected_name}' moved from set ${_expected_set} "
                        "to set ${_actual_set}.")
                endif()
            endif()
        endforeach()
    endif()

    _lumin_abi_json_length(_abi_struct_count _manifest_json shaders ${_shader_index} abiStructs)
    set(_requires_post_process_uniforms FALSE)
    if(_abi_struct_count GREATER 0)
        math(EXPR _last_abi_struct "${_abi_struct_count} - 1")
        foreach(_abi_struct_index RANGE 0 ${_last_abi_struct})
            _lumin_abi_json_get(_abi_struct_name _manifest_json
                shaders ${_shader_index} abiStructs ${_abi_struct_index})
            _lumin_validate_shader_struct(
                "${_shader_name}" _reflection_json _manifest_json "${_abi_struct_name}")
            if(_abi_struct_name STREQUAL "PostProcessUniforms")
                set(_requires_post_process_uniforms TRUE)
            endif()
        endforeach()
    endif()

    if(_requires_post_process_uniforms)
        file(READ "${_source_path}" _shader_source_text)
        if(_shader_source_text MATCHES "struct[ \t\r\n]+PostProcessUniforms")
            message(FATAL_ERROR
                "${_shader_name}: PostProcessUniforms must come from shaders/include/PostProcessUniforms.slang.")
        endif()
        file(READ "${_depfile_path}" _depfile_text)
        string(FIND "${_depfile_text}" "PostProcessUniforms.slang" _common_include_position)
        if(_common_include_position LESS 0)
            message(FATAL_ERROR
                "${_shader_name}: depfile does not track the shared PostProcessUniforms.slang include.")
        endif()
    endif()
endforeach()

message(STATUS
    "Shader ABI validation passed: ${_validated_shader_count}/${_shader_count} enabled entries, "
    "exact bindings, entry stages, and struct layouts.")

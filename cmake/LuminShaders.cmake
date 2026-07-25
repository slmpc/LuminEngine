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

function(lumin_add_slang_shader)
    set(_one_value_args
        OUTPUT_VARIABLE
        SOURCE
        OUTPUT
        ENTRY_POINT
        STAGE
        TARGET
        PROFILE
        MATRIX_LAYOUT
    )
    set(_multi_value_args DEPENDS OPTIONS)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${_one_value_args}" "${_multi_value_args}")

    foreach(_required_arg OUTPUT_VARIABLE SOURCE OUTPUT ENTRY_POINT STAGE)
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

    get_filename_component(_source "${ARG_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_output "${ARG_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(_output_directory "${_output}" DIRECTORY)

    set(_compiler_options
        -target "${ARG_TARGET}"
        -profile "${ARG_PROFILE}"
    )
    if(ARG_TARGET STREQUAL "spirv")
        list(APPEND _compiler_options -fvk-use-entrypoint-name)
    endif()
    if(ARG_MATRIX_LAYOUT)
        list(APPEND _compiler_options "-matrix-layout-${ARG_MATRIX_LAYOUT}")
    endif()
    list(APPEND _compiler_options
        -entry "${ARG_ENTRY_POINT}"
        -stage "${ARG_STAGE}"
        ${ARG_OPTIONS}
    )

    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_directory}"
        COMMAND "${LUMIN_SLANGC_EXECUTABLE}" "${_source}" ${_compiler_options} -o "${_output}"
        DEPENDS "${_source}" ${ARG_DEPENDS}
        COMMENT "Compiling ${ARG_STAGE} shader ${_source}"
        VERBATIM
        COMMAND_EXPAND_LISTS
    )

    set(_outputs "${${ARG_OUTPUT_VARIABLE}}")
    list(APPEND _outputs "${_output}")
    set(${ARG_OUTPUT_VARIABLE} "${_outputs}" PARENT_SCOPE)
endfunction()

function(lumin_add_slang_graphics_shader)
    set(_one_value_args
        OUTPUT_VARIABLE
        SOURCE
        OUTPUT_DIRECTORY
        OUTPUT_NAME
        VERTEX_ENTRY_POINT
        FRAGMENT_ENTRY_POINT
        TARGET
        PROFILE
        MATRIX_LAYOUT
    )
    set(_multi_value_args DEPENDS OPTIONS)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${_one_value_args}" "${_multi_value_args}")

    foreach(_required_arg OUTPUT_VARIABLE SOURCE OUTPUT_DIRECTORY)
        if(NOT ARG_${_required_arg})
            message(FATAL_ERROR "lumin_add_slang_graphics_shader requires ${_required_arg}.")
        endif()
    endforeach()

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "lumin_add_slang_graphics_shader received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT ARG_OUTPUT_NAME)
        get_filename_component(ARG_OUTPUT_NAME "${ARG_SOURCE}" NAME_WE)
    endif()
    if(NOT ARG_VERTEX_ENTRY_POINT)
        set(ARG_VERTEX_ENTRY_POINT vertexMain)
    endif()
    if(NOT ARG_FRAGMENT_ENTRY_POINT)
        set(ARG_FRAGMENT_ENTRY_POINT fragmentMain)
    endif()

    set(_common_args)
    foreach(_arg TARGET PROFILE MATRIX_LAYOUT)
        if(ARG_${_arg})
            list(APPEND _common_args ${_arg} "${ARG_${_arg}}")
        endif()
    endforeach()
    if(ARG_DEPENDS)
        list(APPEND _common_args DEPENDS ${ARG_DEPENDS})
    endif()
    if(ARG_OPTIONS)
        list(APPEND _common_args OPTIONS ${ARG_OPTIONS})
    endif()

    set(_outputs "${${ARG_OUTPUT_VARIABLE}}")
    lumin_add_slang_shader(
        OUTPUT_VARIABLE _outputs
        SOURCE "${ARG_SOURCE}"
        OUTPUT "${ARG_OUTPUT_DIRECTORY}/${ARG_OUTPUT_NAME}.vert.spv"
        ENTRY_POINT "${ARG_VERTEX_ENTRY_POINT}"
        STAGE vertex
        ${_common_args}
    )
    lumin_add_slang_shader(
        OUTPUT_VARIABLE _outputs
        SOURCE "${ARG_SOURCE}"
        OUTPUT "${ARG_OUTPUT_DIRECTORY}/${ARG_OUTPUT_NAME}.frag.spv"
        ENTRY_POINT "${ARG_FRAGMENT_ENTRY_POINT}"
        STAGE fragment
        ${_common_args}
    )

    set(${ARG_OUTPUT_VARIABLE} "${_outputs}" PARENT_SCOPE)
endfunction()

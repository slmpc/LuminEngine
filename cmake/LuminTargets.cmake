include_guard(GLOBAL)

function(lumin_configure_library target)
    target_include_directories(${target}
        PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/core>"
    )
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PUBLIC /utf-8 PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(WIN32)
        target_compile_definitions(${target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()

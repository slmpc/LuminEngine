add_executable(lumin_editor main.cpp)
target_compile_features(lumin_editor PRIVATE cxx_std_23)
target_link_libraries(lumin_editor PRIVATE Lumin::Engine)

if(MSVC)
    target_compile_options(lumin_editor PRIVATE /utf-8)
endif()

add_dependencies(lumin_editor lumin_shaders)
set_target_properties(lumin_editor
    PROPERTIES
        OUTPUT_NAME LuminEngine
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)

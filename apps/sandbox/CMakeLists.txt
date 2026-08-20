add_executable(lumin_sandbox
    main.cpp
    SandboxGame.cpp
)
target_compile_features(lumin_sandbox PRIVATE cxx_std_23)
target_link_libraries(lumin_sandbox PRIVATE Lumin::Engine)

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/apps/sandbox/scripts" LUMIN_SANDBOX_SCRIPT_DIR_CMAKE)
target_compile_definitions(lumin_sandbox
    PRIVATE
        LUMIN_ASSET_DIR="${LUMIN_ASSET_DIR_CMAKE}"
        LUMIN_SANDBOX_SCRIPT_DIR="${LUMIN_SANDBOX_SCRIPT_DIR_CMAKE}"
)
if(MSVC)
    target_compile_options(lumin_sandbox PRIVATE /utf-8)
endif()

add_dependencies(lumin_sandbox lumin_shaders)
set_target_properties(lumin_sandbox
    PROPERTIES
        OUTPUT_NAME LuminEngine
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)

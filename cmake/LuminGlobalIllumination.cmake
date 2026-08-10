target_sources(lumin_render_engine PRIVATE src/render/gi/SsaoBackend.cpp)

if(LUMIN_BUILD_TESTS)
    add_executable(lumin_gi_tests
        tests/GlobalIlluminationTests.cpp
        src/render/FrameGraph.cpp
        src/render/gi/SsaoBackend.cpp
        src/render/PipelineFactory.cpp
        src/render/ShaderLibrary.cpp
    )
    target_link_libraries(lumin_gi_tests PRIVATE Lumin::Runtime nvrhi)
    target_include_directories(lumin_gi_tests PRIVATE src)
    target_compile_definitions(lumin_gi_tests PRIVATE
        LUMIN_GI_TESTING=1
        LUMIN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME GlobalIllumination COMMAND lumin_gi_tests)
endif()

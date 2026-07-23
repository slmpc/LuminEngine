target_sources(lumin_render_engine PRIVATE src/render/DescriptorIndexingLimits.cpp)

if(LUMIN_BUILD_TESTS)
    add_executable(lumin_descriptor_indexing_tests tests/DescriptorIndexingTests.cpp)
    target_include_directories(lumin_descriptor_indexing_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(lumin_descriptor_indexing_tests PRIVATE Lumin::Rendering)
    add_test(NAME DescriptorIndexing COMMAND lumin_descriptor_indexing_tests)
endif()

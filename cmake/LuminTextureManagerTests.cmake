if(LUMIN_BUILD_TESTS)
    add_executable(lumin_texture_manager_tests
        tests/TextureManagerTests.cpp
        src/render/TextureManager.cpp
        src/render/VulkanResources.cpp
    )
    target_include_directories(lumin_texture_manager_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(lumin_texture_manager_tests PRIVATE cxx_std_20)
    target_compile_definitions(lumin_texture_manager_tests PRIVATE
        LUMIN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        LUMIN_TEXTURE_MANAGER_STANDALONE_TEST
    )
    target_link_libraries(lumin_texture_manager_tests PRIVATE glm::glm nvrhi)
    add_test(NAME TextureManager COMMAND lumin_texture_manager_tests)
endif()

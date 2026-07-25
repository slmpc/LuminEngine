if(LUMIN_BUILD_TESTS)
    add_executable(lumin_model_renderer_nvrhi_tests
        tests/ModelRendererNvrhiTests.cpp
        src/render/DescriptorIndexingLimits.cpp
    )
    target_include_directories(lumin_model_renderer_nvrhi_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_features(lumin_model_renderer_nvrhi_tests PRIVATE cxx_std_20)
    target_link_libraries(lumin_model_renderer_nvrhi_tests PRIVATE glm::glm nvrhi)
    add_test(NAME ModelRendererNvrhi COMMAND lumin_model_renderer_nvrhi_tests)
endif()

function(lumin_configure_test target)
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/core"
    )
    target_compile_features(${target} PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()

function(lumin_register_test target test_name)
    set(_options HARDWARE)
    set(_one_value_args RESOURCE_LOCK SKIP_RETURN_CODE TIMEOUT)
    set(_multi_value_args ARGS LABELS)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "${_options}" "${_one_value_args}" "${_multi_value_args}")

    set(_labels ${ARG_LABELS})
    if(ARG_HARDWARE)
        list(APPEND _labels hardware)
        if(NOT ARG_TIMEOUT)
            set(ARG_TIMEOUT 120)
        endif()
        if(NOT ARG_SKIP_RETURN_CODE)
            set(ARG_SKIP_RETURN_CODE 77)
        endif()
        if(NOT ARG_RESOURCE_LOCK)
            set(ARG_RESOURCE_LOCK lumin_gpu)
        endif()
        target_compile_definitions(${target} PRIVATE LUMIN_CTEST_SKIP_RETURN_CODE=${ARG_SKIP_RETURN_CODE})
    else()
        list(APPEND _labels cpu)
    endif()
    list(REMOVE_DUPLICATES _labels)

    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()

    add_test(NAME ${test_name} COMMAND ${target} ${ARG_ARGS})
    set_tests_properties(${test_name} PROPERTIES LABELS "${_labels}" TIMEOUT "${ARG_TIMEOUT}")
    if(ARG_SKIP_RETURN_CODE)
        set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE "${ARG_SKIP_RETURN_CODE}")
    endif()
    if(ARG_RESOURCE_LOCK)
        set_tests_properties(${test_name} PROPERTIES RESOURCE_LOCK "${ARG_RESOURCE_LOCK}")
    endif()
endfunction()

add_executable(lumin_nvrhi_dependency_tests NvrhiDependencyTests.cpp)
lumin_configure_test(lumin_nvrhi_dependency_tests)
target_link_libraries(lumin_nvrhi_dependency_tests PRIVATE nvrhi nvrhi_vk)
lumin_register_test(lumin_nvrhi_dependency_tests NvrhiDependency LABELS nvrhi contract)

add_executable(lumin_nvrhi_backend_tests NvrhiBackendTests.cpp)
lumin_configure_test(lumin_nvrhi_backend_tests)
target_link_libraries(lumin_nvrhi_backend_tests PRIVATE Vulkan::Vulkan nvrhi nvrhi_vk)
lumin_register_test(lumin_nvrhi_backend_tests NvrhiBackend LABELS nvrhi vulkan contract)

add_executable(lumin_vulkan_rt_capability_tests VulkanRayTracingCapabilitiesTests.cpp)
lumin_configure_test(lumin_vulkan_rt_capability_tests)
target_link_libraries(lumin_vulkan_rt_capability_tests PRIVATE Lumin::VulkanBackend)
target_compile_definitions(lumin_vulkan_rt_capability_tests
    PRIVATE
        LUMIN_EXPECTED_RAY_TRACING_MODE=\"${LUMIN_RAY_TRACING}\"
        LUMIN_EXPECTED_RAY_TRACING_AVAILABLE=$<BOOL:${LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE}>
)
lumin_register_test(
    lumin_vulkan_rt_capability_tests
    VulkanRayTracingCapabilities
    LABELS vulkan raytracing fallback unit contract
)

add_executable(lumin_vulkan_hardware_probe_tests VulkanHardwareProbeTests.cpp)
lumin_configure_test(lumin_vulkan_hardware_probe_tests)
target_link_libraries(lumin_vulkan_hardware_probe_tests PRIVATE Vulkan::Vulkan Lumin::VulkanBackend)
lumin_register_test(
    lumin_vulkan_hardware_probe_tests
    Vulkan13HardwareProbe
    HARDWARE
    ARGS --vulkan13
    LABELS vulkan smoke
)
lumin_register_test(
    lumin_vulkan_hardware_probe_tests
    RayTracingHardwareProbe
    HARDWARE
    ARGS --raytracing
    LABELS vulkan raytracing smoke
)
lumin_register_test(
    lumin_vulkan_hardware_probe_tests
    SharcCapabilityHardwareProbe
    HARDWARE
    ARGS --sharc
    LABELS vulkan raytracing sharc smoke
)

add_test(
    NAME RayTracingBuildPolicyConfigure
    COMMAND "${CMAKE_COMMAND}"
            "-DLUMIN_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DLUMIN_TEST_BINARY_DIR=${PROJECT_BINARY_DIR}/tests/ray-tracing-policy"
            "-DLUMIN_TEST_GENERATOR=${CMAKE_GENERATOR}"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyRayTracingBuildPolicy.cmake"
)
set_tests_properties(
    RayTracingBuildPolicyConfigure
    PROPERTIES LABELS "cpu;raytracing;fallback;unit;cmake;contract" TIMEOUT 30
)

add_executable(lumin_frame_graph_tests FrameGraphTests.cpp)
lumin_configure_test(lumin_frame_graph_tests)
target_link_libraries(lumin_frame_graph_tests PRIVATE Lumin::RenderCore)
lumin_register_test(lumin_frame_graph_tests FrameGraphNvRhiManualBarriers LABELS nvrhi render unit contract)

add_executable(lumin_nvrhi_resource_tests NvrhiResourceTests.cpp)
lumin_configure_test(lumin_nvrhi_resource_tests)
target_link_libraries(lumin_nvrhi_resource_tests PRIVATE nvrhi)
lumin_register_test(lumin_nvrhi_resource_tests NvrhiResource LABELS nvrhi contract)

add_executable(lumin_nvrhi_pipeline_tests NvrhiPipelineTests.cpp)
lumin_configure_test(lumin_nvrhi_pipeline_tests)
target_compile_definitions(lumin_nvrhi_pipeline_tests
    PRIVATE LUMIN_TEST_SHADER_DIR="${LUMIN_SHADER_OUTPUT_DIR_CMAKE}"
)
target_link_libraries(lumin_nvrhi_pipeline_tests PRIVATE nvrhi)
add_dependencies(lumin_nvrhi_pipeline_tests lumin_shaders)
lumin_register_test(lumin_nvrhi_pipeline_tests NvrhiPipeline LABELS nvrhi shader contract)

add_executable(lumin_shader_cpu_abi_tests ShaderAbiTests.cpp)
lumin_configure_test(lumin_shader_cpu_abi_tests)
target_link_libraries(lumin_shader_cpu_abi_tests PRIVATE glm::glm nvrhi)
add_dependencies(lumin_shader_cpu_abi_tests lumin_shader_abi)
lumin_register_test(lumin_shader_cpu_abi_tests ShaderCpuAbi LABELS shader raytracing unit contract)

add_test(
    NAME ShaderReflectionAbi
    COMMAND "${CMAKE_COMMAND}"
            "-DLUMIN_SHADER_MANIFEST=${LUMIN_SHADER_MANIFEST}"
            "-DLUMIN_SHADER_REFLECTION_DIR=${LUMIN_SHADER_OUTPUT_DIR}"
            "-DLUMIN_SHADER_SOURCE_DIR=${LUMIN_SHADER_SOURCE_DIR}"
            "-DLUMIN_SHADER_ENABLE_RAY_TRACING=${LUMIN_RAY_TRACING_IMPLEMENTATION_AVAILABLE}"
            "-DLUMIN_SHADER_ENABLE_NRD=${LUMIN_NRD_AVAILABLE}"
            "-DLUMIN_SHADER_ENABLE_SHARC=${LUMIN_SHARC_AVAILABLE}"
            -P "${PROJECT_SOURCE_DIR}/cmake/ValidateShaderAbi.cmake"
)
set_tests_properties(ShaderReflectionAbi PROPERTIES LABELS "cpu;shader;raytracing;unit;contract" TIMEOUT 30)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
    NAME ShaderManifestIncrementalGeneration
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ShaderManifestGeneratorTests.py"
            "${PROJECT_SOURCE_DIR}/scripts/shader_manifest.py"
)
set_tests_properties(
    ShaderManifestIncrementalGeneration
    PROPERTIES LABELS "cpu;shader;build;unit;contract" TIMEOUT 30
)

add_executable(lumin_render_pipeline_contract_tests
    RenderPipelineContractsTests.cpp
    "${PROJECT_SOURCE_DIR}/render/resources/PipelineFactory.cpp"
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderLibrary.cpp"
)
lumin_configure_test(lumin_render_pipeline_contract_tests)
target_link_libraries(lumin_render_pipeline_contract_tests PRIVATE nvrhi)
lumin_register_test(lumin_render_pipeline_contract_tests RenderPipelineContracts LABELS render shader unit contract)

add_executable(lumin_imgui_renderer_tests
    ImGuiRendererTests.cpp
    "${PROJECT_SOURCE_DIR}/render/platform/Window.cpp"
    "${PROJECT_SOURCE_DIR}/render/editor/ImGuiLayer.cpp"
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderLibrary.cpp"
)
lumin_configure_test(lumin_imgui_renderer_tests)
target_link_libraries(lumin_imgui_renderer_tests PRIVATE Vulkan::Vulkan SDL3::SDL3 imgui::imgui nvrhi)
target_compile_definitions(lumin_imgui_renderer_tests PRIVATE LUMIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
lumin_register_test(lumin_imgui_renderer_tests ImGuiRenderer LABELS vulkan ui unit contract)

add_executable(lumin_tests CameraLevelTests.cpp)
lumin_configure_test(lumin_tests)
target_link_libraries(lumin_tests PRIVATE Lumin::Rendering)
target_compile_definitions(lumin_tests
    PRIVATE
        LUMIN_TEST_ASSET_DIR="${LUMIN_ASSET_DIR_CMAKE}"
        LUMIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
lumin_register_test(lumin_tests CameraLevel LABELS scene raster unit)

add_executable(lumin_game_engine_tests GameEngineIntegrationTests.cpp)
lumin_configure_test(lumin_game_engine_tests)
target_link_libraries(lumin_game_engine_tests PRIVATE Lumin::GameEngine)
lumin_register_test(lumin_game_engine_tests GameEngineIntegration LABELS scene integration TIMEOUT 60)

add_executable(lumin_level_renderer_tests LevelRendererTests.cpp)
lumin_configure_test(lumin_level_renderer_tests)
target_compile_definitions(lumin_level_renderer_tests PRIVATE LUMIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
lumin_register_test(lumin_level_renderer_tests LevelRendererRecorder LABELS render raster fallback contract)

add_executable(lumin_render_backend_policy_tests RenderBackendPolicyTests.cpp)
lumin_configure_test(lumin_render_backend_policy_tests)
target_compile_definitions(lumin_render_backend_policy_tests PRIVATE LUMIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
lumin_register_test(lumin_render_backend_policy_tests RenderBackendPolicy LABELS vulkan raytracing fallback contract)

add_executable(lumin_descriptor_indexing_tests DescriptorIndexingTests.cpp)
lumin_configure_test(lumin_descriptor_indexing_tests)
target_link_libraries(lumin_descriptor_indexing_tests PRIVATE Lumin::Rendering)
lumin_register_test(lumin_descriptor_indexing_tests DescriptorIndexing LABELS raster unit contract)

add_executable(lumin_gi_tests
    GlobalIlluminationTests.cpp
    "${PROJECT_SOURCE_DIR}/render/resources/FrameGraph.cpp"
    "${PROJECT_SOURCE_DIR}/render/gi/SsaoBackend.cpp"
    "${PROJECT_SOURCE_DIR}/render/resources/PipelineFactory.cpp"
    "${PROJECT_SOURCE_DIR}/render/resources/ShaderLibrary.cpp"
)
lumin_configure_test(lumin_gi_tests)
target_link_libraries(lumin_gi_tests PRIVATE Lumin::RenderCore nvrhi)
target_compile_definitions(lumin_gi_tests
    PRIVATE
        LUMIN_GI_TESTING=1
        LUMIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
lumin_register_test(lumin_gi_tests GlobalIllumination LABELS gi fallback unit contract)

if(LUMIN_NRD_AVAILABLE)
    add_executable(lumin_gi_composite_tests GiCompositeTests.cpp)
    lumin_configure_test(lumin_gi_composite_tests)
    target_link_libraries(lumin_gi_composite_tests PRIVATE Lumin::RenderFeatures)
    lumin_register_test(lumin_gi_composite_tests GiComposite LABELS gi nrd raytracing unit contract)

    add_executable(lumin_nrd_denoiser_tests NrdDenoiserTests.cpp)
    lumin_configure_test(lumin_nrd_denoiser_tests)
    target_link_libraries(lumin_nrd_denoiser_tests PRIVATE Lumin::RenderFeatures Lumin::Nrd)
    lumin_register_test(lumin_nrd_denoiser_tests NrdDenoiser LABELS nrd raytracing unit contract)
endif()

if(LUMIN_SHARC_AVAILABLE)
    add_executable(lumin_sharc_radiance_cache_tests SharcRadianceCacheTests.cpp)
    lumin_configure_test(lumin_sharc_radiance_cache_tests)
    target_link_libraries(lumin_sharc_radiance_cache_tests PRIVATE Lumin::RenderFeatures)
    lumin_register_test(lumin_sharc_radiance_cache_tests SharcRadianceCache LABELS sharc raytracing unit contract)
endif()

if(LUMIN_NRD_AVAILABLE)
    add_executable(lumin_ray_traced_gi_tests RayTracedGiTests.cpp)
    lumin_configure_test(lumin_ray_traced_gi_tests)
    target_link_libraries(lumin_ray_traced_gi_tests PRIVATE Lumin::RenderFeatures)
    lumin_register_test(lumin_ray_traced_gi_tests RayTracedGi LABELS gi raytracing unit contract)
endif()

add_executable(lumin_model_renderer_nvrhi_tests
    ModelRendererNvrhiTests.cpp
    "${PROJECT_SOURCE_DIR}/render/resources/DescriptorIndexingLimits.cpp"
)
lumin_configure_test(lumin_model_renderer_nvrhi_tests)
target_link_libraries(lumin_model_renderer_nvrhi_tests PRIVATE glm::glm nvrhi)
lumin_register_test(lumin_model_renderer_nvrhi_tests ModelRendererNvrhi LABELS raster unit contract)

add_executable(lumin_scripting_tests ScriptingTests.cpp)
lumin_configure_test(lumin_scripting_tests)
target_link_libraries(lumin_scripting_tests PRIVATE Lumin::Scripting)
lumin_register_test(lumin_scripting_tests Scripting LABELS unit)

add_executable(lumin_texture_manager_tests
    TextureManagerTests.cpp
    "${PROJECT_SOURCE_DIR}/render/resources/TextureManager.cpp"
    "${PROJECT_SOURCE_DIR}/render/resources/VulkanResources.cpp"
)
lumin_configure_test(lumin_texture_manager_tests)
target_compile_definitions(lumin_texture_manager_tests
    PRIVATE
        LUMIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        LUMIN_TEXTURE_MANAGER_STANDALONE_TEST
)
target_link_libraries(lumin_texture_manager_tests PRIVATE glm::glm nvrhi)
lumin_register_test(lumin_texture_manager_tests TextureManager LABELS raster unit contract)

add_executable(lumin_editor_tests EditorTests.cpp)
lumin_configure_test(lumin_editor_tests)
target_link_libraries(lumin_editor_tests PRIVATE Lumin::Editor)
lumin_register_test(lumin_editor_tests Editor LABELS unit)

add_executable(lumin_project_editor_tests ProjectEditorTests.cpp)
lumin_configure_test(lumin_project_editor_tests)
target_link_libraries(lumin_project_editor_tests PRIVATE Lumin::Editor)
lumin_register_test(lumin_project_editor_tests ProjectEditor LABELS scene ui unit)

add_executable(lumin_render_architecture_tests RenderArchitectureTests.cpp)
lumin_configure_test(lumin_render_architecture_tests)
target_link_libraries(lumin_render_architecture_tests PRIVATE Lumin::RenderCore)
lumin_register_test(lumin_render_architecture_tests RenderArchitecture LABELS render unit contract)

add_executable(lumin_atmosphere_architecture_tests AtmosphereArchitectureTests.cpp)
lumin_configure_test(lumin_atmosphere_architecture_tests)
target_link_libraries(lumin_atmosphere_architecture_tests PRIVATE Lumin::Atmosphere)
lumin_register_test(lumin_atmosphere_architecture_tests AtmosphereArchitecture LABELS atmosphere unit contract)

add_executable(lumin_atmosphere_lut_pass_tests AtmosphereLutPassTests.cpp)
lumin_configure_test(lumin_atmosphere_lut_pass_tests)
target_link_libraries(lumin_atmosphere_lut_pass_tests PRIVATE Lumin::Atmosphere)
lumin_register_test(lumin_atmosphere_lut_pass_tests AtmosphereLutPass LABELS atmosphere unit contract)

add_executable(lumin_atmosphere_lut_gpu_tests AtmosphereLutGpuTests.cpp)
lumin_configure_test(lumin_atmosphere_lut_gpu_tests)
target_link_libraries(lumin_atmosphere_lut_gpu_tests PRIVATE Lumin::RenderFeatures)
lumin_register_test(lumin_atmosphere_lut_gpu_tests AtmosphereLutGpu LABELS atmosphere vulkan unit contract)

add_executable(lumin_render_feature_pipeline_tests RenderFeaturePipelineTests.cpp)
lumin_configure_test(lumin_render_feature_pipeline_tests)
target_link_libraries(lumin_render_feature_pipeline_tests PRIVATE Lumin::RenderCore)
lumin_register_test(lumin_render_feature_pipeline_tests RenderFeaturePipeline LABELS render unit contract)

add_executable(lumin_deferred_render_pipeline_tests DeferredRenderPipelineTests.cpp)
lumin_configure_test(lumin_deferred_render_pipeline_tests)
target_link_libraries(lumin_deferred_render_pipeline_tests PRIVATE Lumin::RenderCore)
lumin_register_test(lumin_deferred_render_pipeline_tests DeferredRenderPipeline LABELS render unit contract)

add_executable(lumin_render_world_tests RenderWorldTests.cpp)
lumin_configure_test(lumin_render_world_tests)
target_link_libraries(lumin_render_world_tests PRIVATE Lumin::RenderCore)
lumin_register_test(lumin_render_world_tests RenderWorld LABELS scene unit)

add_executable(lumin_gpu_scene_tests GpuSceneTests.cpp)
lumin_configure_test(lumin_gpu_scene_tests)
target_link_libraries(lumin_gpu_scene_tests PRIVATE Lumin::RenderCore)
lumin_register_test(lumin_gpu_scene_tests GpuScene LABELS raytracing gpu-scene unit contract)

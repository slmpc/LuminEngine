cmake_minimum_required(VERSION 3.25)
project(LuminRayTracingPolicyFixture LANGUAGES NONE)

if(NOT LUMIN_ENGINE_SOURCE_DIR)
    message(FATAL_ERROR "LUMIN_ENGINE_SOURCE_DIR is required.")
endif()

list(APPEND CMAKE_MODULE_PATH "${LUMIN_ENGINE_SOURCE_DIR}/cmake")
include(LuminRayTracing)
lumin_configure_ray_tracing_policy(OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/generated/include")

find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)

add_library(lumin_scripting STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/scripting/ScriptRuntime.cpp"
)
add_library(Lumin::Scripting ALIAS lumin_scripting)
lumin_configure_library(lumin_scripting)
target_include_directories(lumin_scripting PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(lumin_scripting
    PUBLIC
        Lumin::Runtime
    PRIVATE
        sol2::sol2
        ${LUA_LIBRARIES}
)

list(APPEND LUMIN_GAME_ENGINE_FEATURE_LIBRARIES Lumin::Scripting)

if(LUMIN_BUILD_TESTS)
    add_executable(lumin_scripting_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/ScriptingTests.cpp")
    target_link_libraries(lumin_scripting_tests PRIVATE Lumin::Scripting)
    target_compile_features(lumin_scripting_tests PRIVATE cxx_std_20)
    add_test(NAME Scripting COMMAND lumin_scripting_tests)
endif()

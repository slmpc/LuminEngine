add_library(lumin_editor STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/Editor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/EditorStyle.cpp"
)
add_library(Lumin::Editor ALIAS lumin_editor)
lumin_configure_library(lumin_editor)
target_link_libraries(lumin_editor
    PUBLIC
        Lumin::Rendering
        Lumin::Scripting
    PRIVATE
        imgui::imgui
)

list(APPEND LUMIN_GAME_ENGINE_FEATURE_LIBRARIES Lumin::Editor)

if(LUMIN_BUILD_TESTS)
    add_executable(lumin_editor_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/EditorTests.cpp")
    target_link_libraries(lumin_editor_tests PRIVATE Lumin::Editor)
    target_compile_features(lumin_editor_tests PRIVATE cxx_std_20)
    add_test(NAME Editor COMMAND lumin_editor_tests)
endif()

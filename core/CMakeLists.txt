# Core 是纯 CPU 模块：场景、资产、脚本和 Game API 不引入 Vulkan、SDL 或渲染器。
find_package(glm CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(lumin_core STATIC
    ${PROJECT_SOURCE_DIR}/core/assets/ImageLoader.cpp
    ${PROJECT_SOURCE_DIR}/core/assets/ObjLoader.cpp
    ${PROJECT_SOURCE_DIR}/core/project/ProjectSession.cpp
    ${PROJECT_SOURCE_DIR}/core/scene/Camera.cpp
    ${PROJECT_SOURCE_DIR}/core/scene/CameraController.cpp
    ${PROJECT_SOURCE_DIR}/core/scene/Environment.cpp
    ${PROJECT_SOURCE_DIR}/core/scene/Level.cpp
    ${PROJECT_SOURCE_DIR}/core/scripting/ScriptRuntime.cpp
)
add_library(Lumin::Core ALIAS lumin_core)
add_library(Lumin::Runtime ALIAS lumin_core)
add_library(Lumin::Scripting ALIAS lumin_core)
lumin_configure_library(lumin_core)
target_include_directories(lumin_core PUBLIC "${PROJECT_SOURCE_DIR}/core")
target_include_directories(lumin_core PRIVATE "${Stb_INCLUDE_DIR}" "${LUA_INCLUDE_DIR}")
target_link_libraries(lumin_core
    PUBLIC
        glm::glm
        nlohmann_json::nlohmann_json
    PRIVATE
        tinyobjloader::tinyobjloader
        sol2::sol2
        ${LUA_LIBRARIES}
)

# 兼容旧调用方的目标名；新的依赖图只应使用 Lumin::Core。
set_target_properties(lumin_core PROPERTIES EXPORT_NAME Core)

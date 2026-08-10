include_guard(GLOBAL)

# shader_manifest.py 负责 JSON 校验、feature 过滤和 slangc custom command 生成；
# CMake 这里只保留工具发现，避免在脚本语言中重复实现 JSON 解析。
find_program(LUMIN_SLANGC_EXECUTABLE
    NAMES slangc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/Bin32"
    DOC "Path to the Slang shader compiler"
)

if(NOT LUMIN_SLANGC_EXECUTABLE)
    message(FATAL_ERROR "slangc was not found. Install the Vulkan SDK with Slang support or add slangc to PATH.")
endif()

mark_as_advanced(LUMIN_SLANGC_EXECUTABLE)

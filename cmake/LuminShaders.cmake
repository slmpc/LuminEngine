include_guard(GLOBAL)

# C++ ShaderCatalogGenerator 负责类型化配置、feature 过滤和 slangc custom command 生成；
# CMake 这里只保留工具发现与配置期启动。
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

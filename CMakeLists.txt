cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(gdrm-studio VERSION 0.2.0)

# ---------------------------------------------------------------------------
# Dear ImGui as an isolated static library.
# Kept separate from the Geode target on purpose: the mod target gets cocos2d
# include dirs (which bundle a GLEW header). If those leaked into ImGui's
# OpenGL3 backend, GLEW's gl* macros clash with ImGui's built-in imgl3w loader.
# Compiling ImGui in its own target keeps it on the clean Windows GL headers.
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(gdsr_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(gdsr_imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_compile_definitions(gdsr_imgui PUBLIC
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    IMGUI_DEFINE_MATH_OPERATORS
)
# ANGLE/OpenGL ES does not honor GL_UNPACK_ROW_LENGTH, which garbles the dynamic
# font atlas on partial uploads. Force the line-by-line texture upload path.
target_compile_definitions(gdsr_imgui PRIVATE IMGUI_GDSR_NO_UNPACK_ROW_LENGTH)
target_compile_features(gdsr_imgui PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(gdsr_imgui PUBLIC opengl32 dwmapi user32 gdi32 imm32)
endif()

# ---------------------------------------------------------------------------
# The Geode mod.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MOD_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(${PROJECT_NAME} SHARED ${MOD_SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ole32 — COM (CoCreateInstance) for the WASAPI audio capture in AudioCapture.cpp.
# pdh — Performance Counters (CPU usage) for PerformanceMonitor
# Note: UniversalEncoder uses #pragma comment(lib, ...) for dxgi/d3d11 as well,
# but listing them here explicitly is cleaner.
target_link_libraries(${PROJECT_NAME} gdsr_imgui winmm d3d11 dxgi d3dcompiler ole32 pdh)

cmake_minimum_required(VERSION 3.20)
project(XBXViewer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FetchContent ──────────────────────────────────────────
include(FetchContent)

# GLFW
FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# GLAD
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG        v0.1.36
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glad)

# GLM
FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.1
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glm)

# ImGui (with GLFW+OpenGL3 backends)
FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.6
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw)

# ImGuiFileDialog
FetchContent_Declare(imguifiledialog
        GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog.git
        GIT_TAG        v0.6.7
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imguifiledialog)

add_library(imguifiledialog_lib STATIC
        ${imguifiledialog_SOURCE_DIR}/ImGuiFileDialog.cpp)
target_include_directories(imguifiledialog_lib PUBLIC
        ${imguifiledialog_SOURCE_DIR})
target_link_libraries(imguifiledialog_lib PUBLIC imgui_lib)

# STB
FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(stb)

add_library(stb_lib INTERFACE)
target_include_directories(stb_lib INTERFACE ${stb_SOURCE_DIR})

# ── Sources ───────────────────────────────────────────────
set(SOURCES
        src/main.cpp
        src/App.cpp
        src/XBXParser.cpp
        src/Skeleton.cpp
        src/Animation.cpp
        src/Renderer.cpp
        src/Camera.cpp
        src/Texture.cpp
        src/UI.cpp
        src/WorldParser.cpp)

add_executable(XBXViewer ${SOURCES})

target_include_directories(XBXViewer PRIVATE src)

target_link_libraries(XBXViewer PRIVATE
        glfw
        glad
        glm::glm
        imgui_lib
        imguifiledialog_lib
        stb_lib)

# Windows: keep a console window for stdout/stderr logging.
# Use compiler-agnostic approach so it works with both MSVC and MinGW.
if(WIN32)
    if(MSVC)
        target_link_options(XBXViewer PRIVATE /SUBSYSTEM:CONSOLE)
    else()
        # MinGW / GCC: -mconsole sets the PE subsystem to console
        target_link_options(XBXViewer PRIVATE -mconsole)
    endif()
endif()

message(STATUS "XBX Viewer configured. Build with: cmake --build build --config Release")
cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(APP_TARGET Spiderman2AssetBrowser)
set(APP_DISPLAY_NAME "Spiderman 2 Asset Browser")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(${APP_TARGET} VERSION 1.0 DESCRIPTION "${APP_DISPLAY_NAME}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG        v0.1.36
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glad)

FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.1
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glm)

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

FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(stb)

add_library(stb_lib INTERFACE)
target_include_directories(stb_lib INTERFACE ${stb_SOURCE_DIR})

set(SOURCES
        src/core/main.cpp
        src/core/App.cpp
        src/export/ModelExporter.cpp
        src/io/Vfs.cpp
        src/parsers/XBXParser.cpp
        src/parsers/Skeleton.cpp
        src/parsers/Animation.cpp
        src/parsers/WorldParser.cpp
        src/render/Renderer.cpp
        src/render/Camera.cpp
        src/render/Texture.cpp
        src/ui/UI.cpp
)

add_executable(${APP_TARGET} ${SOURCES})
set_target_properties(${APP_TARGET} PROPERTIES
        OUTPUT_NAME "${APP_DISPLAY_NAME}")

target_include_directories(${APP_TARGET} PRIVATE
        src
        src/core
        src/export
        src/io
        src/render
        src/ui
        src/parsers)

target_link_libraries(${APP_TARGET} PRIVATE
        glfw
        glad
        glm::glm
        imgui_lib
        imguifiledialog_lib
        stb_lib)

if(MSVC)
    foreach(TARGET_NAME IN ITEMS glfw glad glm imgui_lib imguifiledialog_lib ${APP_TARGET})
        if(TARGET ${TARGET_NAME})
            set_property(TARGET ${TARGET_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
    target_compile_options(${APP_TARGET} PRIVATE
            $<$<CONFIG:Debug>:/Z7>
            /FS)
endif()

message(STATUS "${APP_DISPLAY_NAME} configured. Build with: cmake --build build --config Release")

cmake_minimum_required(VERSION 3.16)
project(ScreenMirror LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ScreenMirror WIN32
    src/main.cpp
)

target_compile_definitions(ScreenMirror PRIVATE
    WIN32
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_compile_options(ScreenMirror PRIVATE /O2 /W3)

target_link_libraries(ScreenMirror PRIVATE
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    user32.lib
    gdi32.lib
    kernel32.lib
)

# Output to bin/
set_target_properties(ScreenMirror PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
)

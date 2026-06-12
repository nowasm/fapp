cmake_minimum_required(VERSION 3.21)
project(figmalib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FIGMALIB_BACKEND_RAYLIB "Build the raylib backend" ON)
option(FIGMALIB_BUILD_EXAMPLES "Build example apps" ON)

# ThorVG: prebuilt static library (meson: --default-library=static -Dstatic=true).
# Prefer a CPU+GL build (engines=cpu,gl) when present — enables the zero-copy
# GPU path; the CPU-only build still works (GPU calls fall back gracefully).
set(THORVG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/inc" CACHE PATH
    "Directory containing thorvg.h")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/build_static_gl/src/libthorvg-1.a")
    set(_thorvg_default "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/build_static_gl/src/libthorvg-1.a")
else()
    set(_thorvg_default "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/build_static/src/libthorvg-1.a")
endif()
set(THORVG_LIBRARY "${_thorvg_default}" CACHE FILEPATH "Path to the static thorvg library")

if(NOT EXISTS "${THORVG_LIBRARY}")
    message(FATAL_ERROR "ThorVG static library not found at ${THORVG_LIBRARY}. "
        "Build it with: meson setup build_static --default-library=static -Dstatic=true "
        "-Dengines=cpu -Dloaders=svg,lottie,ttf,png,jpg,webp && ninja -C build_static")
endif()

# ThorVG was built /MD (meson release default); match it in every config.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# ---- core library ----
# fig2json CLI used to convert .fig files at load time (override at runtime
# with the FIGMALIB_FIG2JSON environment variable).
set(FIGMALIB_FIG2JSON "${CMAKE_CURRENT_SOURCE_DIR}/../fig2json/target/release/fig2json.exe"
    CACHE FILEPATH "Path to the fig2json executable")

add_library(figmalib STATIC
    src/document.cpp
    src/parser.cpp
    src/canvas_parser.cpp
    src/fig_loader.cpp
    src/serializer.cpp
    src/layout.cpp
    src/svg_path.cpp
    src/font_provider.cpp
    src/scene_builder.cpp
    src/renderer.cpp
    src/ui.cpp
)
target_compile_definitions(figmalib PRIVATE
    FIGMALIB_FIG2JSON_DEFAULT="${FIGMALIB_FIG2JSON}")

# fig2json as a static library → in-process .fig conversion (no subprocess).
# Build it with: cargo build --release (crate-type staticlib). When absent,
# loading falls back to the fig2json CLI.
set(FIGMALIB_FIG2JSON_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../fig2json/target/release/fig2json.lib"
    CACHE FILEPATH "Path to the fig2json static library (empty/missing → CLI fallback)")
if(EXISTS "${FIGMALIB_FIG2JSON_LIB}")
    target_compile_definitions(figmalib PRIVATE FIGMALIB_HAVE_FIG2JSON_LIB)
    target_link_libraries(figmalib PUBLIC "${FIGMALIB_FIG2JSON_LIB}")
    if(WIN32)
        # Rust std dependencies.
        target_link_libraries(figmalib PUBLIC ws2_32 userenv ntdll bcrypt advapi32)
    endif()
endif()
target_include_directories(figmalib
    PUBLIC include "${THORVG_INCLUDE_DIR}"
    PRIVATE src third_party
)
target_compile_definitions(figmalib PUBLIC TVG_STATIC)
target_link_libraries(figmalib PUBLIC "${THORVG_LIBRARY}")
if(WIN32)
    target_link_libraries(figmalib PRIVATE advapi32)  # font registry lookup
endif()

# ---- raylib backend ----
if(FIGMALIB_BACKEND_RAYLIB)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
        URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    )
    FetchContent_MakeAvailable(raylib)

    add_library(figmalib_raylib STATIC backends/raylib/figmalib_raylib.cpp)
    target_include_directories(figmalib_raylib PUBLIC backends/raylib)
    target_link_libraries(figmalib_raylib PUBLIC figmalib raylib)
endif()

# ---- examples ----
if(FIGMALIB_BUILD_EXAMPLES AND FIGMALIB_BACKEND_RAYLIB)
    add_executable(demo_raylib examples/demo_raylib/main.cpp)
    target_link_libraries(demo_raylib PRIVATE figmalib_raylib)
    target_compile_definitions(demo_raylib PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")

    add_executable(demo_wallet examples/demo_wallet/main.cpp)
    target_link_libraries(demo_wallet PRIVATE figmalib_raylib)
endif()

if(FIGMALIB_BUILD_EXAMPLES)
    add_executable(render_test examples/render_test/main.cpp)
    target_link_libraries(render_test PRIVATE figmalib)
    target_compile_definitions(render_test PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")

    add_executable(layout_test examples/layout_test/main.cpp)
    target_link_libraries(layout_test PRIVATE figmalib)
endif()

# ---- editor app ----
if(FIGMALIB_BACKEND_RAYLIB)
    FetchContent_Declare(raygui
        URL https://github.com/raysan5/raygui/archive/refs/tags/4.0.tar.gz
    )
    FetchContent_MakeAvailable(raygui)

    add_executable(figmaedit
        apps/editor/main.cpp
        apps/editor/editor_state.cpp
        apps/editor/editor_canvas.cpp
        apps/editor/editor_panels.cpp
        apps/editor/editor_inspector.cpp
        apps/editor/editor_dialogs.cpp
        apps/editor/editor_mcp.cpp
        apps/editor/editor_mcp_net.cpp
    )
    target_include_directories(figmaedit PRIVATE
        "${raygui_SOURCE_DIR}/src" third_party)
    target_link_libraries(figmaedit PRIVATE figmalib raylib)
    if(WIN32)
        target_link_libraries(figmaedit PRIVATE comdlg32 ws2_32)  # dialogs + MCP socket
    endif()
    target_compile_definitions(figmaedit PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
endif()

cmake_minimum_required(VERSION 3.21)
project(figmalib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FIGMALIB_BACKEND_RAYLIB "Build the raylib backend" ON)
option(FIGMALIB_BUILD_EXAMPLES "Build example apps" ON)
option(FIGMALIB_SCRIPTING "Build the QuickJS scripting host (figmalib_script)" ON)

# ThorVG: prebuilt static library (meson: --default-library=static -Dstatic=true).
# Prefer a CPU+GL build (engines=cpu,gl) when present — enables the zero-copy
# GPU path; the CPU-only build still works (GPU calls fall back gracefully).
# Web builds use the wasm cross build (see tools/build_thorvg_wasm.cmd).
set(THORVG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/inc" CACHE PATH
    "Directory containing thorvg.h")
if(EMSCRIPTEN)
    set(_thorvg_default "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/build_wasm/src/libthorvg-1.a")
elseif(ANDROID)
    if(ANDROID_ABI STREQUAL "x86_64")
        set(_thorvg_default "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/build_android_x64/src/libthorvg-1.a")
    else()
        set(_thorvg_default "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/build_android_arm64/src/libthorvg-1.a")
    endif()
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../thorvg/build_static_gl/src/libthorvg-1.a")
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

# Emscripten disables C++ exceptions by default (throw would abort), but the
# parsers rely on them — use native wasm exceptions (fast, Chrome/Edge 95+).
if(EMSCRIPTEN)
    add_compile_options(-fwasm-exceptions)
    add_link_options(-fwasm-exceptions)
endif()

# Android: everything links into one shared library (the NativeActivity
# loads libfigmaplay.so), so every static archive must be PIC.
if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Desktop-only pieces: tests, demos, the editor, .fig conversion.
if(EMSCRIPTEN OR ANDROID)
    set(FIGMALIB_DESKTOP OFF)
else()
    set(FIGMALIB_DESKTOP ON)
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
if(EXISTS "${FIGMALIB_FIG2JSON_LIB}" AND WIN32)  # a Windows static lib
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

# ---- scripting host (QuickJS) ----
# App logic becomes <design.fig> + <logic.js>: figmalib::ScriptHost binds the
# FigmaUI API into a QuickJS context (see include/figmalib/script.h).
if(FIGMALIB_SCRIPTING)
    include(FetchContent)
    set(BUILD_QJS_LIBC OFF CACHE BOOL "" FORCE)         # we bind our own API
    set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_CLI_WITH_MIMALLOC OFF CACHE BOOL "" FORCE)
    set(QJS_DISABLE_PARSER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(quickjs
        URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.15.1.tar.gz
    )
    FetchContent_MakeAvailable(quickjs)

    add_library(figmalib_script STATIC src/script_host.cpp)
    target_include_directories(figmalib_script PRIVATE third_party)  # nlohmann
    target_link_libraries(figmalib_script PUBLIC figmalib qjs)
    if(WIN32)
        target_link_libraries(figmalib_script PRIVATE winhttp)  # fetch()
    endif()
endif()

# ---- raylib backend ----
if(FIGMALIB_BACKEND_RAYLIB)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    if(EMSCRIPTEN)
        set(PLATFORM "Web" CACHE STRING "" FORCE)  # raylib: GLFW shim + WebGL
    elseif(ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)  # NativeActivity + EGL/GLES2
    endif()
    FetchContent_Declare(raylib
        URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    )
    FetchContent_MakeAvailable(raylib)

    add_library(figmalib_raylib STATIC backends/raylib/figmalib_raylib.cpp)
    target_include_directories(figmalib_raylib PUBLIC backends/raylib)
    target_link_libraries(figmalib_raylib PUBLIC figmalib raylib)
endif()

# ---- examples ----
if(FIGMALIB_BUILD_EXAMPLES AND FIGMALIB_BACKEND_RAYLIB AND FIGMALIB_DESKTOP)
    add_executable(demo_raylib examples/demo_raylib/main.cpp)
    target_link_libraries(demo_raylib PRIVATE figmalib_raylib)
    target_compile_definitions(demo_raylib PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")

    add_executable(demo_wallet examples/demo_wallet/main.cpp)
    target_link_libraries(demo_wallet PRIVATE figmalib_raylib)
endif()

# ---- tests ----
if(FIGMALIB_BUILD_EXAMPLES AND FIGMALIB_DESKTOP)
    add_executable(render_test tests/render_test/main.cpp)
    target_link_libraries(render_test PRIVATE figmalib)
    target_compile_definitions(render_test PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")

    add_executable(layout_test tests/layout_test/main.cpp)
    target_link_libraries(layout_test PRIVATE figmalib)
endif()

# ---- fapp2godot: design (.fig / canvas.json / REST) -> Godot .tscn ----
if(FIGMALIB_DESKTOP)
    add_executable(fapp2godot apps/fapp2godot/main.cpp)
    target_link_libraries(fapp2godot PRIVATE figmalib)
    target_include_directories(fapp2godot PRIVATE third_party)  # nlohmann (manifest, later)
endif()

# ---- figmaplay: the generic script player (an app = .fig + .js) ----
if(FIGMALIB_SCRIPTING AND FIGMALIB_BACKEND_RAYLIB)
    if(ANDROID)
        # The NativeActivity loads libfigmaplay.so; keep the glue's entry
        # point alive (raylib only sets that flag in its own scope).
        add_library(figmaplay SHARED apps/figmaplay/main.cpp)
        target_include_directories(figmaplay PRIVATE
            "${ANDROID_NDK}/sources/android/native_app_glue")
        target_link_options(figmaplay PRIVATE "SHELL:-u ANativeActivity_onCreate")
        target_link_libraries(figmaplay PRIVATE android log)
    else()
        add_executable(figmaplay apps/figmaplay/main.cpp)
    endif()
    target_link_libraries(figmaplay PRIVATE figmalib_raylib figmalib_script)
    target_include_directories(figmaplay PRIVATE third_party)  # nlohmann (app.json)
    target_compile_definitions(figmaplay PRIVATE
        EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
    if(EMSCRIPTEN)
        set_target_properties(figmaplay PROPERTIES SUFFIX ".html")
        target_link_options(figmaplay PRIVATE
            "-sUSE_GLFW=3"
            "-sALLOW_MEMORY_GROWTH=1"
            "-sSTACK_SIZE=2097152"
            "-sFORCE_FILESYSTEM=1"
            "-sFETCH"  # script-layer fetch() → browser XHR (async, no pthreads)
        )
        # A packaged app (figmapack): a standard app dir preloaded at /app, which
        # the runtime prefers over the bundled wallet. The design must already be
        # canvas.json / REST json (figmapack converts .fig first).
        set(FIGMALIB_WEB_APP_DIR "" CACHE PATH "Packaged app dir preloaded at /app")
        if(FIGMALIB_WEB_APP_DIR)
            # SHELL: keeps each flag+arg pair intact (CMake de-dups bare flags)
            target_link_options(figmaplay PRIVATE
                "SHELL:--preload-file ${FIGMALIB_WEB_APP_DIR}@/app")
        else()
            # Default: the bundled wallet demo. The design ships pre-converted
            # (canvas.json + images/) — .fig conversion is a native-only step.
            set(FIGMALIB_WEB_DESIGN_DIR
                "${CMAKE_CURRENT_SOURCE_DIR}/../fig2psd/test/figma/wallet.fig.export"
                CACHE PATH "Design dir (canvas.json + images) preloaded into the web FS")
            target_link_options(figmaplay PRIVATE
                "SHELL:--preload-file ${FIGMALIB_WEB_DESIGN_DIR}@/assets/wallet"
                "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/scripts/wallet.js@/scripts/wallet.js"
                "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts@/fonts")
        endif()
    endif()
    if(WIN32 AND NOT EMSCRIPTEN AND NOT ANDROID)
        # figmapack sets this per app to embed the app's icon into figmaplay.exe.
        set(FIGMALIB_WIN_ICON "" CACHE FILEPATH "A .ico embedded into figmaplay.exe")
        if(FIGMALIB_WIN_ICON)
            set(_figmaplay_rc "${CMAKE_CURRENT_BINARY_DIR}/figmaplay_icon.rc")
            file(WRITE "${_figmaplay_rc}" "IDI_ICON1 ICON \"${FIGMALIB_WIN_ICON}\"\n")
            target_sources(figmaplay PRIVATE "${_figmaplay_rc}")
        endif()
    endif()
endif()

# ---- editor app ----
if(FIGMALIB_BACKEND_RAYLIB AND FIGMALIB_DESKTOP)
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
        backends/raylib/pinch_stub.cpp   # trackpad pinch source (empty on Apple)
    )
    target_include_directories(figmaedit PRIVATE
        "${raygui_SOURCE_DIR}/src" third_party backends/raylib)
    target_link_libraries(figmaedit PRIVATE figmalib raylib)
    if(WIN32)
        target_link_libraries(figmaedit PRIVATE comdlg32 ws2_32)  # dialogs + MCP socket
    endif()
    if(APPLE)
        # Two-finger pinch zoom: a Cocoa NSEventMaskMagnify monitor that GLFW
        # does not expose (see backends/raylib/pinch_macos.mm).
        target_sources(figmaedit PRIVATE backends/raylib/pinch_macos.mm)
        target_link_libraries(figmaedit PRIVATE "-framework Cocoa")
    endif()
    target_compile_definitions(figmaedit PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
endif()

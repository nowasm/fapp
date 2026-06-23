cmake_minimum_required(VERSION 3.21)
project(figo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FIGO_BACKEND_RAYLIB "Build the raylib backend" ON)
option(FIGO_BUILD_EXAMPLES "Build example apps" ON)
option(FIGO_SCRIPTING "Build the QuickJS scripting host (figo_script)" ON)

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
# loads libfigoplay.so), so every static archive must be PIC.
if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Desktop-only pieces: tests, demos, the editor, .fig conversion.
if(EMSCRIPTEN OR ANDROID)
    set(FIGO_DESKTOP OFF)
else()
    set(FIGO_DESKTOP ON)
endif()

# ---- core library ----
# fig2json CLI used to convert .fig files at load time (override at runtime
# with the FIGO_FIG2JSON environment variable).
set(FIGO_FIG2JSON "${CMAKE_CURRENT_SOURCE_DIR}/../fig2json/target/release/fig2json.exe"
    CACHE FILEPATH "Path to the fig2json executable")

add_library(figo STATIC
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
target_compile_definitions(figo PRIVATE
    FIGO_FIG2JSON_DEFAULT="${FIGO_FIG2JSON}")

# fig2json as a static library → in-process .fig conversion (no subprocess).
# Build it with: cargo build --release (crate-type staticlib). When absent,
# loading falls back to the fig2json CLI.
set(FIGO_FIG2JSON_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../fig2json/target/release/fig2json.lib"
    CACHE FILEPATH "Path to the fig2json static library (empty/missing → CLI fallback)")
if(EXISTS "${FIGO_FIG2JSON_LIB}" AND WIN32)  # a Windows static lib
    target_compile_definitions(figo PRIVATE FIGO_HAVE_FIG2JSON_LIB)
    target_link_libraries(figo PUBLIC "${FIGO_FIG2JSON_LIB}")
    if(WIN32)
        # Rust std dependencies.
        target_link_libraries(figo PUBLIC ws2_32 userenv ntdll bcrypt advapi32)
    endif()
endif()
target_include_directories(figo
    PUBLIC include "${THORVG_INCLUDE_DIR}"
    PRIVATE src third_party
)
target_compile_definitions(figo PUBLIC TVG_STATIC)
target_link_libraries(figo PUBLIC "${THORVG_LIBRARY}")
if(WIN32)
    target_link_libraries(figo PRIVATE advapi32)  # font registry lookup
endif()

# ---- scripting host (QuickJS) ----
# App logic becomes <design.fig> + <logic.js>: figo::ScriptHost binds the
# FigmaUI API into a QuickJS context (see include/figo/script.h).
if(FIGO_SCRIPTING)
    include(FetchContent)
    set(BUILD_QJS_LIBC OFF CACHE BOOL "" FORCE)         # we bind our own API
    set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_CLI_WITH_MIMALLOC OFF CACHE BOOL "" FORCE)
    set(QJS_DISABLE_PARSER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(quickjs
        URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.15.1.tar.gz
    )
    FetchContent_MakeAvailable(quickjs)

    add_library(figo_script STATIC src/script_host.cpp)
    target_include_directories(figo_script PRIVATE third_party)  # nlohmann
    target_link_libraries(figo_script PUBLIC figo qjs)
    if(WIN32)
        target_link_libraries(figo_script PRIVATE winhttp)  # fetch()
    endif()
endif()

# ---- raylib backend ----
if(FIGO_BACKEND_RAYLIB)
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

    add_library(figo_raylib STATIC backends/raylib/figo_raylib.cpp)
    target_include_directories(figo_raylib PUBLIC backends/raylib)
    target_link_libraries(figo_raylib PUBLIC figo raylib)
endif()

# ---- examples ----
if(FIGO_BUILD_EXAMPLES AND FIGO_BACKEND_RAYLIB AND FIGO_DESKTOP)
    add_executable(demo_raylib examples/demo_raylib/main.cpp)
    target_link_libraries(demo_raylib PRIVATE figo_raylib)
    target_compile_definitions(demo_raylib PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")

    add_executable(demo_wallet examples/demo_wallet/main.cpp)
    target_link_libraries(demo_wallet PRIVATE figo_raylib)
endif()

# ---- tests ----
if(FIGO_BUILD_EXAMPLES AND FIGO_DESKTOP)
    add_executable(render_test tests/render_test/main.cpp)
    target_link_libraries(render_test PRIVATE figo)
    target_compile_definitions(render_test PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")

    add_executable(layout_test tests/layout_test/main.cpp)
    target_link_libraries(layout_test PRIVATE figo)
endif()

# ---- figo2godot: design (.fig / canvas.json / REST) -> Godot .tscn ----
if(FIGO_DESKTOP)
    add_executable(figo2godot apps/figo2godot/main.cpp)
    target_link_libraries(figo2godot PRIVATE figo)
    target_include_directories(figo2godot PRIVATE third_party)  # nlohmann (manifest, later)
endif()

# ---- figo2cocos: design (.fig / canvas.json / REST) -> Cocos Creator .prefab ----
if(FIGO_DESKTOP)
    add_executable(figo2cocos apps/figo2cocos/main.cpp)
    target_link_libraries(figo2cocos PRIVATE figo)
    target_include_directories(figo2cocos PRIVATE third_party)  # nlohmann (prefab JSON)
endif()

# ---- figoplay: the generic script player (an app = .fig + .js) ----
if(FIGO_SCRIPTING AND FIGO_BACKEND_RAYLIB)
    if(ANDROID)
        # The NativeActivity loads libfigoplay.so; keep the glue's entry
        # point alive (raylib only sets that flag in its own scope).
        add_library(figoplay SHARED apps/figoplay/main.cpp)
        target_include_directories(figoplay PRIVATE
            "${ANDROID_NDK}/sources/android/native_app_glue")
        target_link_options(figoplay PRIVATE "SHELL:-u ANativeActivity_onCreate")
        target_link_libraries(figoplay PRIVATE android log)
    else()
        add_executable(figoplay apps/figoplay/main.cpp)
    endif()
    target_link_libraries(figoplay PRIVATE figo_raylib figo_script)
    target_include_directories(figoplay PRIVATE third_party)  # nlohmann (app.json)
    target_compile_definitions(figoplay PRIVATE
        EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
    if(EMSCRIPTEN)
        set_target_properties(figoplay PROPERTIES SUFFIX ".html")
        target_link_options(figoplay PRIVATE
            "-sUSE_GLFW=3"
            "-sALLOW_MEMORY_GROWTH=1"
            "-sSTACK_SIZE=2097152"
            "-sFORCE_FILESYSTEM=1"
            "-sFETCH"  # script-layer fetch() → browser XHR (async, no pthreads)
        )
        # A packaged app (figmapack): a standard app dir preloaded at /app, which
        # the runtime prefers over the bundled wallet. The design must already be
        # canvas.json / REST json (figmapack converts .fig first).
        set(FIGO_WEB_APP_DIR "" CACHE PATH "Packaged app dir preloaded at /app")
        if(FIGO_WEB_APP_DIR)
            # SHELL: keeps each flag+arg pair intact (CMake de-dups bare flags)
            target_link_options(figoplay PRIVATE
                "SHELL:--preload-file ${FIGO_WEB_APP_DIR}@/app")
        else()
            # Default: the bundled wallet demo. The design ships pre-converted
            # (canvas.json + images/) — .fig conversion is a native-only step.
            set(FIGO_WEB_DESIGN_DIR
                "${CMAKE_CURRENT_SOURCE_DIR}/../fig2psd/test/figma/wallet.fig.export"
                CACHE PATH "Design dir (canvas.json + images) preloaded into the web FS")
            target_link_options(figoplay PRIVATE
                "SHELL:--preload-file ${FIGO_WEB_DESIGN_DIR}@/assets/wallet"
                "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/scripts/wallet.js@/scripts/wallet.js"
                "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts@/fonts")
        endif()
    endif()
    if(WIN32 AND NOT EMSCRIPTEN AND NOT ANDROID)
        # figmapack sets this per app to embed the app's icon into figoplay.exe.
        set(FIGO_WIN_ICON "" CACHE FILEPATH "A .ico embedded into figoplay.exe")
        if(FIGO_WIN_ICON)
            set(_figoplay_rc "${CMAKE_CURRENT_BINARY_DIR}/figoplay_icon.rc")
            file(WRITE "${_figoplay_rc}" "IDI_ICON1 ICON \"${FIGO_WIN_ICON}\"\n")
            target_sources(figoplay PRIVATE "${_figoplay_rc}")
        endif()
    endif()
endif()

# ---- editor app ----
if(FIGO_BACKEND_RAYLIB AND FIGO_DESKTOP)
    FetchContent_Declare(raygui
        URL https://github.com/raysan5/raygui/archive/refs/tags/4.0.tar.gz
    )
    FetchContent_MakeAvailable(raygui)

    add_executable(figoedit
        apps/editor/main.cpp
        apps/editor/editor_state.cpp
        apps/editor/editor_canvas.cpp
        apps/editor/editor_panels.cpp
        apps/editor/editor_inspector.cpp
        apps/editor/editor_dialogs.cpp
        apps/editor/editor_mcp.cpp
        apps/editor/editor_mcp_net.cpp
        apps/editor/svg_import.cpp
        backends/raylib/pinch_stub.cpp   # trackpad pinch source (empty on Apple)
    )
    target_include_directories(figoedit PRIVATE
        "${raygui_SOURCE_DIR}/src" third_party backends/raylib
        "${CMAKE_CURRENT_SOURCE_DIR}/src")  # svg_import reuses src/svg_path.h
    target_link_libraries(figoedit PRIVATE figo raylib)
    if(WIN32)
        target_link_libraries(figoedit PRIVATE comdlg32 ws2_32)  # dialogs + MCP socket
    endif()
    if(APPLE)
        # Two-finger pinch zoom: a Cocoa NSEventMaskMagnify monitor that GLFW
        # does not expose (see backends/raylib/pinch_macos.mm).
        target_sources(figoedit PRIVATE backends/raylib/pinch_macos.mm)
        target_link_libraries(figoedit PRIVATE "-framework Cocoa")
    endif()
    target_compile_definitions(figoedit PRIVATE
        ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
endif()

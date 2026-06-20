@echo off
rem Web build: figoplay.html (wallet demo) into build_web\.
rem Prereqs: emsdk (default D:\devlib\emsdk, override with EMSDK_HOME) and
rem the ThorVG wasm build (tools\build_thorvg_wasm.cmd).
setlocal enabledelayedexpansion
if "%EMSDK_HOME%"=="" set EMSDK_HOME=D:\devlib\emsdk
call %EMSDK_HOME%\emsdk_env.bat >nul 2>&1
cd /d D:\work_open\figo
if not exist build_web\build.ninja (
    rem Reuse the desktop build's fetched sources when present (offline-safe).
    set EXTRA=
    if exist build\_deps\raylib-src\CMakeLists.txt set EXTRA=!EXTRA! -DFETCHCONTENT_SOURCE_DIR_RAYLIB=D:/work_open/figo/build/_deps/raylib-src
    if exist build\_deps\quickjs-src\CMakeLists.txt set EXTRA=!EXTRA! -DFETCHCONTENT_SOURCE_DIR_QUICKJS=D:/work_open/figo/build/_deps/quickjs-src
    call emcmake cmake -B build_web -G Ninja -DCMAKE_BUILD_TYPE=Release !EXTRA!
    if errorlevel 1 exit /b 1
)
ninja -C build_web figoplay

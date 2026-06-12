@echo off
rem Builds ThorVG (software engine, no threads, no lottie) for Android into
rem ..\thorvg\build_android_arm64 and ..\thorvg\build_android_x64.
setlocal
cd /d D:\work_open\thorvg
for %%A in (arm64 x64) do (
    if not exist build_android_%%A\build.ninja (
        meson setup build_android_%%A --cross-file D:\work_open\figmalib\tools\android_%%A.txt --buildtype=release --default-library=static -Dstatic=true -Dthreads=false -Dsimd=false -Dengines=cpu -Dloaders=svg,ttf,png,jpg,webp -Dextra= -Dtools= -Dtests=false
        if errorlevel 1 exit /b 1
    )
    ninja -C build_android_%%A
    if errorlevel 1 exit /b 1
)

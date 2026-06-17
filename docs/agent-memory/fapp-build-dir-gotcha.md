---
name: fapp-build-dir-gotcha
description: "fapp's build/ CMake cache points at a stale sibling source path (D:/work_open/figmalib); use build_godot or reconfigure"
metadata: 
  node_type: memory
  type: project
  originSessionId: 57c1f992-5ebc-462d-96f1-e97a0a9615eb
---

The live repo is `D:\work_open\fapp` (git remote nowasm/fapp). But `D:\work_open\fapp\build\` was configured with `CMAKE_HOME_DIRECTORY = d:/work_open/figmalib` — a STALE sibling copy (figmalib is not a git repo, lacks recent work). Building in `build/` fails: "source directory D:/work_open/figmalib does not appear to contain CMakeLists.txt". The CLAUDE.md build snippet also cd's to `D:\work_open\figmalib\build` — outdated.

Shared siblings (resolved relative to source dir, same for both fapp and figmalib): ThorVG prebuilt static lib at `D:\work_open\thorvg\build_static[_gl]\src\libthorvg-1.a`, fig2json at `D:\work_open\fig2json`.

To build a core-only tool (e.g. fapp2godot — needs just the `figmalib` static lib, no raylib/quickjs), configure a fresh minimal dir pointing at the real source:
```
cmake -S D:\work_open\fapp -B D:\work_open\fapp\build_godot -G Ninja \
  -DFIGMALIB_BACKEND_RAYLIB=OFF -DFIGMALIB_SCRIPTING=OFF -DFIGMALIB_BUILD_EXAMPLES=OFF
cmake --build build_godot --target fapp2godot -j
```
This skips the heavy FetchContent deps (raylib/quickjs/raygui) — only compiles ~12 core cpp + the tool (~seconds). Must run under vcvars64 (MSVC /MD to match ThorVG). Use the PowerShell CRLF+ascii bw.cmd wrapper from CLAUDE.md, but cd to D:\work_open\fapp not figmalib. Related: [[react-to-psd-pipeline]].

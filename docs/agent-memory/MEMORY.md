# Memory Index

- [figmaplay --shot basename gotcha](figmaplay-shot-basename.md) — --shot saves to cwd by basename; JS API can't set fill/size
- [fapp build dir gotcha](fapp-build-dir-gotcha.md) — build/ cache points at stale figmalib path; use a fresh build_godot configured against D:\work_open\fapp
- [React→Godot pipeline (BUILT)](react-to-psd-pipeline.md) — shipped: `tools/web2canvas` (React/HTML→canvas.json, Playwright) + `apps/fapp2godot` (canvas.json→Godot 4 .tscn, native C++/ThorVG) + `html2godot.js` one-command. Semantic naming, dedup, fonts, multi-screen, --prefabs reuse. Validated on GOGO KILL. Full fix log + gotchas inside.

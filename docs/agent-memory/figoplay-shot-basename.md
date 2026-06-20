---
name: figoplay-shot-basename
description: "figoplay --shot saves the screenshot to cwd using only the path's basename"
metadata: 
  node_type: memory
  type: project
  originSessionId: 40190ae3-fbc7-471c-ac02-7c0b8fecc623
---

`figoplay.exe <appdir> --shot <out.png>` (and `--selfdrive`) ignore the
directory part of the output path — raylib's `TakeScreenshot` writes
`<basename>` into the **current working directory**, not the path you pass.

**Why:** cost 3 failed render attempts when batch-rendering frames; an absolute
out path "succeeded" (rc=0, log said it saved) but the file landed in cwd root.

**How to apply:** to render into the app dir, run the subprocess with
`cwd=<appdir>` and pass a bare basename (`--shot _shot_X.png`). To render a
specific frame, temporarily set `app.json.entryFrame` then restore it.

Related: the QuickJS node API (see `include/figmalib/script.h`) can set
`.text/.visible/.opacity/.name` but **not** fill color or size — bake per-color
/ sized variants into design.json instead of mutating them from script.
See [[amongus-test-app]].

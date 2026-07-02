# figo

A C++ vector-UI library that renders **Figma designs (and React/HTML pages)
directly as game UI** inside a game engine.

> 中文版见 [README_CN.md](README_CN.md).

Vector rasterization is powered by [ThorVG](https://github.com/thorvg/thorvg);
the demo backend uses [raylib](https://github.com/raysan5/raylib). The core
library is fully decoupled from any engine — anything that can upload an RGBA
texture and forward mouse events (a Unity native plugin, Unreal, Godot, a custom
engine) can integrate it in a few dozen lines.

Every input is normalized to a single intermediate representation,
**canvas.json**, which the core library then renders or exports:

```
input                                  IR                    output
.fig file ──fig2json──────────┐
Figma REST JSON (?geometry=paths) ─┤
React/HTML ──web2canvas───────┤── canvas.json ─▶ figo::Document ─┬─▶ ThorVG raster ─▶ engine texture
       (captured after real-browser render) │  (format auto-detected) (Frame/Text/…) │   (raylib / Unity / custom runtime…)
                                                                      └─▶ figo2godot ─▶ Godot 4 project
                                                                              (.tscn + PNG sprites)
```

Runtime path (ThorVG):

```
node tree  figo::Document (Frame / Rect / Ellipse / Vector / Text / ...)
        │  scene_builder
        ▼
ThorVG scene graph ──SwCanvas──▶ RGBA8888 pixel buffer (straight alpha)
        │                          │
        ▼                          ▼
   hit-testing / input / callbacks   engine backend uploads as a texture
```

## Directory layout

```
include/figo/   public API (document / parser / renderer / ui / script)
src/                parser, SVG path parsing, ThorVG scene building, fonts, renderer, script host
backends/raylib/    raylib backend (reference template for other engine backends, ~200 lines)
apps/editor/        figoedit — a Figma-style visual editor (raylib + raygui + MCP)
apps/figoplay/     figoplay — a generic script player (app = .fig + .js, hot-reload)
apps/figo2godot/    figo2godot — canvas.json → a Godot 4 project (.tscn + sprites)
apps/figo2cocos/    figo2cocos — canvas.json → Cocos Creator 3.x prefabs (.prefab + textures)
examples/           demo_raylib / demo_wallet demos, example scripts and design files
  assets/           Figma JSON and .fig used for tests/demos
  scripts/          figoplay example scripts (wallet.js)
tools/              figmanew / figmapack / gen_templates (Python), web2canvas (Node)
  web2canvas/       React/HTML → canvas.json (Playwright driving a real browser)
tests/              render_test (offscreen render self-check), layout_test (layout-math self-test)
third_party/        nlohmann/json single header
```

## figoedit editor

```
build\figoedit.exe [design.fig | canvas.json | file.json]   # or drag-and-drop a file onto the window
build\figoedit.exe --selftest [file]                        # headless logic self-test
```

Interactions mirror Figma:

| Action | Shortcut |
|---|---|
| Pan canvas | Space+drag / middle-drag / scroll wheel (Shift = horizontal) |
| Zoom | two-finger pinch (macOS trackpad, cursor-centered), Ctrl+wheel (cursor-centered), Ctrl+= / Ctrl+-, Ctrl+0 = 100%, Shift+1 = fit |
| Select | click top object, Ctrl+click deep-select, double-click to enter container, Esc to exit, Shift+click add/remove, drag empty space to marquee |
| Move | drag (Shift = axis lock), arrow keys to nudge (Shift = 10px) |
| Resize object | corner/edge handles (Shift = uniform, Alt = symmetric about center) |
| Edit | Ctrl+D duplicate, Delete, Ctrl+Z / Ctrl+Shift+Z undo/redo, Ctrl+S save |
| Tools | V move, H hand |

A layer tree on the left (expand / toggle visibility / click to select), an
inspector on the right (X/Y/W/H, opacity, corner radius, fill color, text
content), and a toolbar on top (tools, page switch, zoom level). Saving writes
`<original>.figo.json` (REST format, directly re-loadable by figo, never
overwriting the original .fig). HiDPI scaling is auto-detected and overridable
via the `FIGOEDIT_SCALE` environment variable.

### MCP server (AI designs directly inside the editor)

figoedit embeds an MCP server on startup (Streamable HTTP, bound to localhost
`127.0.0.1:9223` only, endpoint `/mcp`). Once an AI client connects it can read
the layer tree, add/edit/delete nodes, and screenshot to self-check — all edits
share the user's single undo history (Ctrl+Z undoes any AI edit).

```
build\figoedit.exe --mcp-port 9300 design.fig    # change port
build\figoedit.exe --no-mcp design.fig           # disable MCP
set FIGOEDIT_MCP_PORT=9300                        # same via env var
```

Connecting from Claude Code (a `.mcp.json` ships at the repo root and is
auto-discovered inside this project):

```
claude mcp add --transport http figoedit http://127.0.0.1:9223/mcp
```

Tools: `get_editor_state` / `get_node_tree` / `get_node` (read);
`create_node` / `update_nodes` / `delete_nodes` / `duplicate_node` /
`move_node` (write, a batch update = one undo); `get_screenshot` (offscreen
render of the current page or any node to PNG — the AI's "eyes");
`set_selection` / `set_page` (drive the editor UI); `save_document` /
`open_document` / `undo` / `redo`. Node references accept an id or a unique
layer name; colors are `#RRGGBB`/`#RRGGBBAA`; solid/gradient fills, strokes,
shadow/blur effects, text styles, constraints and auto-layout metadata are
supported, and VECTOR nodes can be drawn straight from an SVG path
(`svgPath`).

Tool calls run on the main thread between frames (the document needs no mutex);
when a native file dialog is open a call waits and returns "editor busy" on
timeout. The protocol layer (initialize / tools/list) is answered directly on
the network thread, so a handshake works at any time. `--selftest` includes a
headless regression over the full MCP toolset.

## Build (Windows / MSVC)

1. Build the ThorVG static library (sibling directory `../thorvg`):

   ```
   meson setup build_static_gl --default-library=static -Dstatic=true ^
       -Dengines=cpu,gl -Dloaders=svg,lottie,ttf,png,jpg,webp -Dbuildtype=release
   ninja -C build_static_gl
   ```

   `engines=cpu,gl` enables both software rasterization and the OpenGL engine
   (the zero-copy GPU path); drop `,gl` for CPU-only rendering (GPU calls fall
   back automatically).

2. Build figo (raylib is fetched automatically via CMake FetchContent):

   ```
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   build\demo_raylib.exe                       # built-in example UI
   build\demo_raylib.exe path\to\design.fig    # any .fig / canvas.json / REST JSON
   build\demo_raylib.exe --gpu [file]          # ThorVG GL engine renders straight to an FBO (zero-copy)
   build\render_test.exe [file]                # offscreen render self-check, outputs BMP
   build\layout_test.exe                       # layout-engine math self-test
   ```

   In the demo, ←/→ switch between frames in the design, R toggles responsive
   reflow vs. uniform scaling, G toggles GPU/CPU rendering;
   `--screenshot out.png` renders 30 frames then exits (for automated checks).

The ThorVG paths can be overridden with
`-DTHORVG_INCLUDE_DIR=... -DTHORVG_LIBRARY=...`.

## Usage

```cpp
#include <figo/figo.h>
#include <figo_raylib.h>

auto ui = figo::FigmaUI::fromFile("menu.json");   // Figma REST JSON
ui->onClick("btn-start", [&](figo::Node&, float, float) { startGame(); });
ui->onHover("btn-start", [&](figo::Node& n, bool in, float, float) {
    ui->setOpacity(n.name, in ? 0.8f : -1.0f);
});

figo::RaylibFigmaView view(*ui);
while (!WindowShouldClose()) {
    view.resize(GetScreenWidth(), GetScreenHeight());
    view.update();                       // input + dirty-region redraw + texture upload
    BeginDrawing();
    drawGameWorld();                     // the game shows through translucent UI areas
    view.draw();
    EndDrawing();
}
```

### Scripting layer (QuickJS)

You can build an app without writing C++: **the design is a .fig, the logic is
a .js**. `figo_script` binds the FigmaUI API into QuickJS (quickjs-ng,
fetched by CMake), and `figoplay` is the generic player:

```
figoplay wallet.fig wallet.js
```

```js
// wallet.js — full API in include/figo/script.h
ui.setResizeMode("reflow");
ui.selectFrame("Home");
ui.bindList("portfolio-list", coins.length, (item, i) => {
    item.find("Heading").child(0).text = coins[i].symbol;
});
ui.onClick("Card", (node) => {
    if (node.parent.name !== "portfolio-list") return;
    ui.navigateTo("Coin Info", "slideLeft", 0.28);
});
console.log("ready —", ui.frameNames().length, "frames");
```

`examples/scripts/wallet.js` reproduces all of demo_wallet's behavior in pure
script (data binding, navigation, bottom bar, editable text, design-data
patching). The host only loads the two files and runs the frame loop
(`host.update(dt)` drives `ui.onUpdate` and the JS task queue).

### App projects (a standard directory)

Beyond loose files, an app can be **a directory + `app.json`** — gathering the
design, logic, viewport, fonts, design system and packaging metadata into one
project you hand directly to figoplay:

```
figoplay examples/apps/sample            # reads sample/app.json
```

```jsonc
// examples/apps/sample/app.json
{
  "name": "Starfall Menu",
  "design": "design.json",      // relative to this dir; .fig / canvas.json / REST JSON all OK
  "script": "app.js",
  "viewport": [420, 900],        // window size
  "entryFrame": "MainMenu",      // frame selected at startup (the script can still navigate)
  "fonts": "fonts",              // optional; for platforms without system fonts
  "designSystem": "linear-app",  // optional; points at aesthetic tokens under design-systems/
  "package": { "id": "com.figo.sample", "version": "1.0.0" }  // reserved for packaging
}
```

The manifest is exposed to scripts as `globalThis.APP` (`APP.name` /
`APP.entryFrame` …). Passing two loose files instead of a directory still works
exactly as before (backward compatible). The `designSystem` field wires the
aesthetic knowledge in [`design-systems/`](design-systems/) into the authoring
loop — AI reads the matching tokens before building a design; see
`design-systems/TOKEN_MAPPING.md`.

### Starting from a template (figmanew)

`templates/` holds a few standard app-project templates; `tools/figmanew.py`
copies a template into a new project and fixes up `app.json` — AI / humans start
from a **working app** and edit, rather than generate from zero:

```
python tools/figmanew.py --list                       # list templates
python tools/figmanew.py myapp --template list-detail  # create myapp/
python tools/figmanew.py myapp -t tab-shell -n "My App" -d revolut
figoplay myapp                                        # run it
```

| Template | Structure | Palette from |
|---|---|---|
| `tab-shell` | bottom-tab shell (Home/Search/Profile, fixed nav bar) | linear-app |
| `list-detail` | scrollable list → detail page (`bindList` + navigation) | coinbase |
| `form` | editable inputs + submit (`setEditable`/`focusText`) | stripe |

A template's `design.json` is generated by `tools/gen_templates.py` from the
chosen design system's tokens (palette/radius from `design-tokens.json`).
Template text uses Segoe UI; to restore the design system's own fonts, drop the
corresponding .ttf into the app's `fonts/` directory.

### Getting Figma data

**Option 1: a local .fig file (recommended, offline)**

In Figma, File → Save local copy... to save a `.fig`, then directly:

```cpp
auto ui = figo::FigmaUI::fromFile("design.fig");
```

figo does the .fig→JSON conversion **in-process**
([fig2json](https://github.com/kreako/fig2json) compiled to a static library and
linked directly; CMake `FIGO_FIG2JSON_LIB` points at the .lib produced by
`cargo build --release`), with no external process dependency; the result is
cached in `design.fig.export/` (reused when the .fig is unchanged), and images
are decoded from the .fig's embedded data and fed to the renderer automatically.
When the static lib is absent it falls back to invoking the fig2json CLI (lookup
order: `FIGO_FIG2JSON` env var → CMake `FIGO_FIG2JSON` default path →
PATH). You can also pre-convert manually (`fig2json design.fig outdir`) and load
`outdir/canvas.json` directly.

The canvas.json parser is ported from [fig2psd](../fig2psd)'s normalization
logic: type-structure inference for nodes/fills/effects, component instances
completed by cloning from the master (including symbolOverrides and shared-style
resolution), derivedSymbolData geometry-fallback baking, vectorNetwork
stitching, inverse-matrix gradient geometry conversion.

**Option 2: REST API**

```
GET https://api.figma.com/v1/files/<FILE_KEY>?geometry=paths
    -H "X-Figma-Token: <PERSONAL_ACCESS_TOKEN>"
```

Save the returned JSON to a file and `FigmaUI::fromFile` can load it.
`geometry=paths` includes vector outlines (`fillGeometry`/`strokeGeometry`), so
INSIDE/OUTSIDE stroke alignment is reproduced precisely. Image fills must be
downloaded separately (`/v1/images`) into the directory given to
`Renderer::setImageDirectory`, named by `imageRef`.

## Supported Figma features

- Nodes: FRAME / GROUP / RECTANGLE / ELLIPSE / LINE / VECTOR / STAR / POLYGON /
  BOOLEAN_OPERATION / TEXT / COMPONENT / INSTANCE
- Fills: solid, linear/radial gradients, angular (conic)/diamond gradients
  (programmatically rasterized into a bitmap fill from the handle basis vectors,
  supporting rotation/eccentricity/non-uniform stretch), image (FILL/FIT/STRETCH)
- Strokes: weight, dashes, cap/join styles, INSIDE/OUTSIDE/CENTER alignment
  (clip/mask simulation; exact when `strokeGeometry` is present)
- Corner radius (incl. per-corner radii), layer opacity, `clipsContent` clipping
- Effects: drop shadow (DROP_SHADOW), inner shadow (INNER_SHADOW, simulated by
  inverse-shape blur + mask, no spread), layer blur (LAYER_BLUR)
- Text: font-directory registration (parsing the TTF name/OS/2 tables) + system
  font matching (Windows registry, including unpacking/recombining .ttc
  collections), Figma line-box vertical layout (lineHeight + half-leading),
  alignment, letter-spacing, auto-wrap, ellipsis truncation (textTruncation),
  multi-line rich text (greedy word wrapping + explicit breaks + mixed-size
  baseline alignment + per-character CJK breaking + non-ASCII segmentation,
  with precise UTF-16↔UTF-8 index mapping), glyph-level font fallback (cmap
  coverage queries — CJK/symbols inside a Latin font are rendered with a system
  fallback font such as YaHei/SimHei)
- Font convention: a `fonts/` directory next to the input file and the
  `FIGO_FONTS_DIR` env var are auto-registered
- Runtime: frame switching, hit-testing, hover/click callbacks, dynamic
  visibility/opacity/text changes
- **Component variant switching**: `ui->setVariant("btn-start", "State", "Hover")`
  switches an instance to the variant matching those properties within the same
  component set (COMPONENT_SET / .fig STATE_GROUP) — it clones the target
  variant subtree, reflows it to the instance size (constraints/auto-layout
  apply), and composes with hover/click callbacks for polymorphic buttons;
  property names/values are case-insensitive, and the component set must be in
  the document
- **Responsive layout**: constraints (LEFT/RIGHT/CENTER/STRETCH/SCALE on both
  axes) + auto-layout (horizontal/vertical stack, padding, spacing, grow,
  alignment, hug sizing, SPACE_BETWEEN, WRAP, BASELINE approximation, min/max
  size limits, absolutely-positioned children); after `setResizeMode(Reflow)`
  frames reflow to the viewport instead of scaling uniformly:

  ```cpp
  ui->setResizeMode(figo::FigmaUI::ResizeMode::Reflow);  // default is Scale
  ui->setViewport(w, h);   // triggers layoutFrame() reflow (toggle with R in the demo)
  ```

  Reflow is based on the original geometry snapshotted at parse time
  (`Node::base*`), so repeated resizing never accumulates error; the layout
  engine is also usable standalone: `figo::layoutFrame(frame, w, h)`. Vector
  path geometry scales with node size, and rounded rects/ellipses are
  regenerated at the new size (corners don't distort). Note: the .fig path needs
  this repo's patched fig2json (the upstream version strips
  constraints/stack fields); stale caches re-convert automatically when fig2json
  updates. `layout_test.exe` is the headless self-test for the layout math.

## React/HTML → Godot project (web2canvas + figo2godot)

Beyond Figma, **a React/HTML page can be turned directly into a Godot 4
project**. The page is rendered in a real headless browser and then captured, so
flex/grid layout, `oklch` colors, `clip-path` cut-corners, gradients, shadows
and font widths all come from what the browser actually computed; anything a
flat fill can't express (gradients / images / inline `<svg>` / clip-path /
rotated elements / dashed borders / glows) is rasterized into a PNG `IMAGE`
fill, in line with the "vector → texture" goal. The whole chain is validated
end-to-end on a real page (the GOGO KILL HUD, 8 screens).

```
React/HTML ──web2canvas──▶ canvas.json + images/ ──figo2godot──▶ Godot .tscn + sprites
```

### web2canvas (`tools/web2canvas/`, Node)

Captures the rendered DOM into a canvas.json. It drives the **installed
Edge/Chrome** (playwright-core, no Chromium download) and bundles
react / react-dom / @babel/standalone (so Babel-in-browser non-ESM React apps
load too; CDN requests are answered from the bundled copies, and local files are
served over a throwaway HTTP server so Babel's XHR can load .jsx).

```
cd tools/web2canvas && npm install
node index.js <url|file.html> [-o out.canvas.json] [--root SEL] [--viewport WxH] \
     [--states "a,b,c"] [--nav-fn FN] [--fonts DIR] [--browser msedge|chrome] [--wait MS]
```

| Flag | Meaning |
|---|---|
| `--root SEL` | element to capture (e.g. `#stage`); default `body` |
| `--viewport WxH` | browser viewport; match the design's stage size for 1:1 |
| `--states "a,b,c"` | multi-screen: calls `window.<navFn>(state)` per state, one top-level frame each |
| `--nav-fn FN` | the global nav function name (default `__nav`) |
| `--fonts DIR` | inject the project's `fonts.css` so text is measured at real widths |

### figo2godot (`apps/figo2godot/`, C++)

Converts a canvas.json into a Godot 4 project: each top-level frame → one
`.tscn`, with deduplicated PNG sprites (content-hash dedup), bound fonts, a
`manifest.json` and a `project.godot`. Node mapping: TEXT→Label, solid
rect/container→ColorRect, rounded panel→NinePatchRect+sprite,
ellipse/vector/gradient/image/stroke/effect→TextureRect+baked sprite (sprites
are rendered by `Renderer::renderOverlay`, pixel-identical to the runtime).
Responsive constraints → Godot anchor/offset. The tool links only the figo
core static library (no raylib/quickjs needed), so it builds in seconds in a
minimal `build_godot` directory.

```
figo2godot <input.canvas.json|.fig|REST.json> [outDir] [--fonts DIR] [--prefabs] [--scale N]
```

`--prefabs`: extract repeated components (cards/buttons/rows) into
`components/*.tscn` (PackedScenes), instancing each occurrence with per-instance
text overrides — real prefab reuse rather than inlined copies.

### figo2cocos (`apps/figo2cocos/`, C++)

Converts a canvas.json into Cocos Creator 3.x assets: each top-level frame → one
self-contained `.prefab` (a JSON array of `__id__`-linked engine objects:
`cc.Prefab`/`cc.Node`/`cc.UITransform`/`cc.Sprite`/`cc.Label`…, the format
ported from psd2prefab), plus a shared `textures/` folder of deduplicated PNG
sprites each with a `.meta` sprite-frame. Node mapping: TEXT→Label, solid
rect/container bg→Sprite over a shared 1×1 `white.png` tinted via `_color`,
ellipse/vector/gradient/image/stroke/effect/rounded→Sprite + baked PNG (9-slice
when large & stretchable). Coordinate flip: anchor `(0,1)`, `_lpos=(relX,-relY)`,
UITransform size = the figo box. Sprites are rendered by `Renderer::renderOverlay`,
pixel-identical to the runtime; UUIDs are content-derived so reruns are stable.

```
figo2cocos <input.canvas.json|.fig|REST.json> [outDir] [--frame NAME] [--scale N]
```

Drop `outDir` (prefabs + `.meta` + `textures/`) into a Cocos Creator 3.x
project's `assets/`. v1 inlines everything (no nested-prefab extraction), uses
system fonts, and is axis-aligned — matching psd2prefab's output shape. Driven
by the **`/figo2cocos` skill** (`.claude/skills/figo2cocos/`).

### One command (html2godot)

```
node tools/web2canvas/html2godot.js <url|file.html> --out <godotDir> \
     [--states "a,b,c"] [--fonts DIR] [--root SEL] [--viewport WxH] [--wait MS] [--prefabs]
```

Runs web2canvas → figo2godot, with intermediate artifacts in
`<godotDir>/.web2canvas/`; `<godotDir>/` opens directly in Godot 4. Full example
(GOGO KILL HUD, 8 screens → 8 scenes):

```
node tools/web2canvas/html2godot.js "<...>/HUD C.html" --out hud_app \
  --root "#stage" --viewport 1280x720 --wait 2500 \
  --fonts "<...>/fonts" --states "lobby,search,room,role,game,meeting,victory,aftermath"
```

> AI workflow: the `web-to-godot` skill (`.claude/skills/web-to-godot/`) wraps
> this chain into an end-to-end flow (set up env → one command → open in Godot
> and screenshot to self-verify → iterate).

**Known limits**: only screen-level capture (`window.__nav`) — popups/overlays
opened only by a click, and per-screen content scrolled beyond the viewport, are
not captured yet; letter-spacing (Godot Label has no native support) and
per-corner NinePatch radii (the max corner is used). Google Fonts are blocked
during capture — use `--fonts` to point at local faces.

## One-command multi-platform packaging (figmapack)

`tools/figmapack.py` packs a standard app project (app.json) into per-platform
bundles in one command:

```
python tools/figmapack.py <app-dir> --target win|web|android|all [--out dist]
python tools/figmapack.py examples/apps/sample -t all
```

Output lands in `<out>/<app-slug>/<target>/`:
- **win**: `figoplay.exe` + `app/` + `run.cmd` (double-click to run)
- **web**: `index.html` + wasm/js/data (serve with `python -m http.server`)
- **android**: a signed `<app>.apk` (`adb install -r`)

Packaging metadata comes from app.json's `package` section:
- `id` → android package name, `version` → versionName/Code, `name` → app
  name / web title.
- `icon` (square PNG) → web favicon + apple-touch-icon, android per-density
  `mipmap/ic_launcher`, and on **win the icon is embedded into the exe** (via
  a .rc relink of figoplay, requiring a configured `build/` + VS environment;
  the `VCVARS` env var can override the path).
- `splashColor` (`#rrggbb`, optional) + `icon` → a **splash screen**: web
  injects a branded loading overlay (icon + app name + spinner, fading out after
  load), android uses a theme windowBackground (base color + centered icon)
  shown during NativeActivity startup.

How it works: figmapack stages the app directory, and the web/android runtimes
**prefer the staged `app.json`** (falling back to the wallet demo if absent), so
all three platforms share one design + logic.

Conventions and gotchas:
- A **.fig design** is converted to canvas.json by fig2json at pack time
  (web/android can't convert on the fly); canvas.json / REST `.json` are packed
  as-is.
- **web/android have no system fonts**: any font used by the app's design must
  be placed in the app's `fonts/` directory, or text renders blank (desktop uses
  system fonts and is unaffected).
- **iOS/macOS are not included here** — they need a Mac + Xcode + codesign;
  app.json's `package` section is reserved for them, pending a `--target ios`
  once a Mac/CI is available.

The low-level details of each platform's build (which figmapack converges from)
follow:

## Web build (emscripten)

The wallet demo can compile to wasm and run in a browser (CPU rasterization →
WebGL presentation):

```
tools\build_thorvg_wasm.cmd    # ThorVG wasm static lib (sw engine, no threads, no lottie)
tools\build_web.cmd            # produces build_web\figoplay.{html,js,wasm,data}
python -m http.server 8123 -d build_web   # open http://localhost:8123/figoplay.html
```

emsdk defaults to `D:\devlib\emsdk` (override with `EMSDK_HOME`). The design is
packed into the virtual filesystem as pre-converted canvas.json + images (.fig
conversion is a native step), and fonts are packed from
`examples/assets/fonts` (the browser has no system fonts; the Titillium
Web/Poppins used by wallet are bundled). The script layer's `fetch()` goes
through emscripten Fetch (`-sFETCH`, async XHR) for real network requests —
same-origin is unrestricted, cross-origin is subject to browser CORS (the server
must send `Access-Control-Allow-Origin`). Limitation: hot-reload / `--shot` work
on desktop only.

## Android build (no gradle)

A NativeActivity (`hasCode=false`) loads libfigoplay.so directly, with aapt
packaging by hand:

```
tools\build_thorvg_android.cmd      # ThorVG arm64-v8a + x86_64 static libs
powershell tools\build_android.ps1  # NDK dual-ABI compile → build_android\figoplay.apk
adb install -r build_android\figoplay.apk
adb shell am start -n com.figo.play/android.app.NativeActivity
```

SDK/NDK come from `D:\devlib\android\sdk` (NDK 27.2, API 28+). Assets inside the
APK are not files; on startup they're unpacked to internal storage per a
generated manifest.txt and then read via normal file IO; design/script/fonts are
the same as the Web build. Touch maps to a pointer (via raylib), and
scroll/inertia/transitions/editing all work.

## GPU rendering

An engine backend can let ThorVG's GL engine render straight into its own FBO
(zero CPU pixel copy):

```cpp
// CPU (default): ui->setViewport(w, h) + ui->pixels() to upload a texture
// GPU: under the current GL context, hand the FBO id to figo
if (!ui->setViewportGL(fboId, w, h)) ui->setViewport(w, h);  // fall back when there's no GL engine
ui->render();  // draws straight into the FBO, no pixels() readback
```

The raylib backend wraps this as `view.setGpu(true)` (internally a
RenderTexture2D, flipping Y when drawing and restoring rlgl's cached state after
ThorVG changes GL state).

## Known limitations

- Layout-time text measurement (text reflow doesn't change the node box height;
  BASELINE approximates the baseline from the font style)
- Prototype interactions; library component variants (when the component set
  isn't in the document)
- BACKGROUND_BLUR (frosted glass needs background sampling); the spread
  parameter of INNER_SHADOW; angular/diamond gradients on strokes (degrade to
  radial)

# web2canvas — React/HTML → figmalib canvas.json → Godot

Turn a rendered React/HTML page into a figmalib `canvas.json`, and (with
`fapp2godot`) into an openable Godot 4 project. The page is rendered in a real
headless browser, so computed CSS — flex/grid layout, `oklch` colors,
`clip-path` cut-corners, gradients, fonts — is captured accurately. Anything a
flat fill can't express (gradients, images, inline `<svg>`, clip-path, rotated
elements, dashed borders, glows) is rasterized to a PNG `IMAGE` fill, matching
the project's vector→texture goal.

```
React/HTML ──web2canvas──► canvas.json + images/ ──fapp2godot──► Godot .tscn + sprites
```

## Install

```
cd tools/web2canvas
npm install         # playwright-core + vendored react/react-dom/@babel/standalone
```

Drives the **installed Edge/Chrome** (no Chromium download). Needs `fapp2godot`
built (see repo root; the core-only `build_godot` target is enough).

## One command: html → Godot project

```
node html2godot.js <url|file.html> --out <godotDir> \
     [--states "a,b,c"] [--fonts DIR] [--root SEL] [--viewport WxH] \
     [--wait MS] [--browser msedge|chrome] [--prefabs] [--ai-name] [--fapp2godot <exe>]
```

`<godotDir>/` becomes an openable Godot 4 project: one `.tscn` per screen,
deduped sprites, bundled fonts, `manifest.json`, `project.godot`.

Add `--prefabs` to extract repeated components (cards, buttons, rows) into
reusable `components/*.tscn` (PackedScenes) and instance them per screen with
per-instance text overrides — real prefab reuse, not just inlined copies.

Add `--ai-name` to name components by **what they look like** instead of their
React/CSS class — a vision pass screenshots each component and asks the `claude`
CLI to infer a `PascalCase` name (see [Component names](#component-names--ai-name)).

## Just the canvas.json

```
node index.js <url|file.html> [-o out.canvas.json] [--root SEL]
     [--viewport WxH] [--states "a,b,c"] [--flows FILE] [--nav-fn FN]
     [--fonts DIR] [--browser msedge|chrome] [--wait MS] [--scale N]
```

| flag | meaning |
|---|---|
| `--root SEL` | element to capture (e.g. `#stage`); default `body` |
| `--viewport WxH` | browser viewport; match the design's stage size for 1:1 |
| `--states "a,b,c"` | capture multiple screens — calls `window.<navFn>(state)` per state, one top-level frame each |
| `--flows FILE` | capture click-driven popups/overlays — a JSON array of captures, each with interaction steps (see below) |
| `--nav-fn FN` | the global nav function (default `__nav`) |
| `--nav-reset S` | sentinel state used to remount between captures (default `__w2c_reset__`) |
| `--fonts DIR` | serve the project's `fonts.css` so text is measured at real widths |
| `--ai-name` | name components from their rendered look via the `claude` CLI (vision) — see below |
| `--scale N` | rasterization supersample (default 2) |

## Popups & overlays: click-driven flows (`--flows`)

`--states` only reaches screens behind a `window.__nav` hook. Popups, drawers and
overlays opened **by clicking** need `--flows FILE` — a JSON array where each
capture optionally navigates, then runs interaction **steps** before the
screenshot. Each capture becomes one top-level frame -> one `.tscn`.

```json
[
  { "name": "game",          "nav": "game" },
  { "name": "game_settings", "nav": "game", "do": ["click:[title=\"游戏设置\"]"] },
  { "name": "game_death",    "nav": "game", "do": ["click:[title^=\"调试\"]", "click:text=模拟被击杀"] }
]
```

- `nav` (optional): the screen to navigate to first via `window.<navFn>`.
- `do` / `steps`: a list of `verb:arg` steps run in order —
  `click:<sel>`, `hover:<sel>` (Playwright selectors: `text=…`, CSS, `[attr=…]`),
  `nav:<state>`, `wait:<ms>`. A bare string defaults to `click`.
- Isolation is automatic: before each capture the target screen is remounted via
  a sentinel state, so a popup opened in one capture never leaks into the next.

A click-triggered second-level page is captured as **just what the click opened**,
not the parent screen behind it: before the steps run, every existing element is
tagged; afterwards the frame is collected from the root of the largest NEW
subtree (a new element whose parent already existed). That catches both a
positioned overlay covering ≥25% of the root (a chat/settings modal) and an
inline panel that REPLACES a sibling (the gift panel swapping the chat bar — a
2–90% chunk). A step that merely mutates the current screen (a toggle) adds no
such subtree, so the full screen is kept.

## Example — GOGO KILL HUD (8 screens → Godot)

```
node html2godot.js "<...>/html_ui_export/app/HUD C.html" --out hud_app \
  --root "#stage" --viewport 1280x720 --wait 2500 \
  --fonts "<...>/html_ui_export/fonts" \
  --states "lobby,search,room,role,game,meeting,victory,aftermath"
```

## Text node names = semantic roles

TEXT nodes are named by an inferred **role**, not their literal text (which is
unstable and meaningless as a node id). `textRole()` classifies by content +
context into: `username` (a short, punctuation-free string next to a portrait),
`playerId`, `seatLabel`, `levelText`, `status`, `count` / `unit`, `amount`,
`percent`, `icon`, `heading`, `buttonLabel` (text inside a `<button>`),
`hintText` (long/sentence copy), else `labelText`. Duplicate sibling names get a
`_2`/`_3` suffix downstream. The "near a portrait → username" signal uses
`isAvatarish` (img/canvas/`image-slot`, a circular ≤72px element, or a square
photo) — reliable on clean roster/list UIs; in dense HUDs full of circular
icons/emblems a few labels may still be tagged `username`. The keyword sets in
`textRole()` are the place to tune the vocabulary per project.

## Component names (`--ai-name`)

Without it, a container's name comes from a **rule**: its nearest React
component (via fiber), else its first CSS class, else the tag (`div_0`). That's
only as meaningful as the source markup — minified/utility classes and
Babel-in-browser apps often yield `div_0`, `css-1abc`, or generic primitives.

`--ai-name` replaces those with names inferred from **how the component
actually looks**. The flow (in `index.js`):

1. While collecting each screen, container nodes worth naming are tagged as
   *candidates* — a top-level screen region, or a substantive component (≥3
   descendants), bounded in size (not the whole frame, not a sub-text fragment).
2. Each candidate is screenshotted **live, in its own screen** (the elements
   only exist in the DOM while their screen is mounted).
3. Structurally-identical candidates are deduped (so 11 identical player cards
   are named once), the representatives are laid out in a numbered montage, and
   the `claude` CLI is asked to name each from its picture
   (`PlayerCard`, `HealthBar`, `CreateRoomButton`, `DailyTaskCard`, …).
4. The name is written onto every member of each dedup group → it flows to the
   `.tscn` node name, the sprite filename, and the `--prefabs` component scene.

Notes:

- **Hybrid, not all-or-nothing**: only candidates are AI-named; everything else
  keeps its rule-based name. A good React component name (`PortraitC`, `Emblem`)
  is left alone when that node isn't a candidate.
- **Needs the `claude` CLI** on `PATH` (headless `claude -p`, reads the montage
  via the `Read` tool). If a call fails or returns no JSON, that batch silently
  falls back to rule-based names — the run still succeeds.
- **Cost**: ~1 `claude` call per 24 unique components (one montage each), a few
  seconds apiece. GOGO KILL's 8 screens = 228 candidates → 119 unique → 5 calls.
- Intermediate montages + candidate shots are written under
  `<out>/.ai-name/` (or `<godotDir>/.ai-name/` via `html2godot`) for inspection.

## Notes / limits

- **CDN apps**: unpkg/jsdelivr requests are served from the vendored
  `node_modules` (byte-identical, so SRI `integrity` passes); Google Fonts are
  aborted (pass `--fonts` to use the project's local faces instead). Local files
  are served over a throwaway HTTP server so Babel's XHR module loading works.
- **Multi-screen** needs a programmatic nav hook (`window.__nav`). Click-driven
  popups/overlays are captured via `--flows` (above). Per-screen scroll content
  beyond the viewport is not captured yet.
- Each raster is screenshotted **in isolation** (everything else hidden) so
  foreground content never bleeds into a background sprite.

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
     [--wait MS] [--browser msedge|chrome] [--fapp2godot <exe>]
```

`<godotDir>/` becomes an openable Godot 4 project: one `.tscn` per screen,
deduped sprites, bundled fonts, `manifest.json`, `project.godot`.

## Just the canvas.json

```
node index.js <url|file.html> [-o out.canvas.json] [--root SEL]
     [--viewport WxH] [--states "a,b,c"] [--nav-fn FN] [--fonts DIR]
     [--browser msedge|chrome] [--wait MS] [--scale N]
```

| flag | meaning |
|---|---|
| `--root SEL` | element to capture (e.g. `#stage`); default `body` |
| `--viewport WxH` | browser viewport; match the design's stage size for 1:1 |
| `--states "a,b,c"` | capture multiple screens — calls `window.<navFn>(state)` per state, one top-level frame each |
| `--nav-fn FN` | the global nav function (default `__nav`) |
| `--fonts DIR` | serve the project's `fonts.css` so text is measured at real widths |
| `--scale N` | rasterization supersample (default 2) |

## Example — GOGO KILL HUD (8 screens → Godot)

```
node html2godot.js "<...>/html_ui_export/app/HUD C.html" --out hud_app \
  --root "#stage" --viewport 1280x720 --wait 2500 \
  --fonts "<...>/html_ui_export/fonts" \
  --states "lobby,search,room,role,game,meeting,victory,aftermath"
```

## Notes / limits

- **CDN apps**: unpkg/jsdelivr requests are served from the vendored
  `node_modules` (byte-identical, so SRI `integrity` passes); Google Fonts are
  aborted (pass `--fonts` to use the project's local faces instead). Local files
  are served over a throwaway HTTP server so Babel's XHR module loading works.
- **Multi-screen** needs a programmatic nav hook (`window.__nav`). Popups/
  overlays opened only by clicks, and per-screen scroll content beyond the
  viewport, are not captured yet.
- Each raster is screenshotted **in isolation** (everything else hidden) so
  foreground content never bleeds into a background sprite.

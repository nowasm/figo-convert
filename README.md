# figo-convert

Converters that turn a **Figma design or a React/HTML page into game-engine
prefabs** — split out of [figo](https://github.com/nowasm/figo), which stays
focused on the runtime (render Figma designs as live game UI + JS scripting).

```
input                                IR                     output
.fig file ────────────────┐
Figma REST JSON (?geometry=paths) ──┤── canvas.json ──┬─▶ figo2godot  ─▶ Godot 4 project (.tscn + sprites)
React/HTML ──web2canvas────┘                       ├─▶ figo2cocos  ─▶ Cocos Creator 3.x prefabs
        (captured after real-browser render)          └─▶ figo2unity ─▶ Unity UGUI prefabs
```

All three converters link the **figo core library** (parse → layout → ThorVG
raster) from the sibling `../figo` repo; sprites are baked by
`Renderer::renderOverlay`, pixel-identical to the figo runtime.

## Layout

```
apps/figo2godot/    canvas.json → a Godot 4 project (.tscn + sprites)
apps/figo2cocos/    canvas.json → Cocos Creator 3.x prefabs (.prefab + textures + .meta)
apps/figo2unity/    canvas.json → Unity UGUI prefabs (.prefab YAML + textures + .meta)
apps/exporter_png.h shared sprite-baking / PNG export helpers
apps/anim_tracks.h  shared animation-track extraction
tools/web2canvas/   React/HTML → canvas.json (Node, Playwright driving Edge/Chrome)
examples/html/      starfall_menu.html sample (menu + settings, 1280×720) + cached canvas.json
skills/             Claude Code skills: figo2cocos / figo2unity / web-to-godot
docs/agent-memory/  pipeline build notes & gotchas
```

## Build (Windows / MSVC)

Prerequisites — sibling repos, same as building figo itself:

- `../figo` — the figo repo (core library sources)
- `../thorvg` — ThorVG static lib (see figo's README for the meson commands);
  override with `-DTHORVG_INCLUDE_DIR=... -DTHORVG_LIBRARY=...`
- `../fig2json` (optional) — direct `.fig` input; without it pass canvas.json / REST JSON

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

With raylib/quickjs disabled (this project forces them OFF for the figo
subdirectory) the whole configure+build takes seconds — only the figo core
`.cpp` files plus the three tools are compiled. On Windows run under
`vcvars64` (MSVC `/MD`, matching ThorVG).

## Usage

```
figo2godot <input.canvas.json|.fig|REST.json> [outDir] [--fonts DIR] [--prefabs] [--scale N]
figo2cocos <input.canvas.json|.fig|REST.json> [outDir] [--frame NAME] [--fonts DIR]
           [--scale N] [--prefabs] [--prefab-anon] [--no-prefab T1,T2]
figo2unity <input.canvas.json|.fig|REST.json> [outDir] [--frame NAME] [--fonts DIR]
           [--scale N] [--prefabs] [--prefab-anon] [--no-prefab T1,T2] [--linear]
```

- **figo2godot** — each top-level frame → one `.tscn`, deduplicated PNG
  sprites, bound fonts, `manifest.json`, `project.godot`. `--prefabs` extracts
  repeated components into `components/*.tscn` PackedScenes with per-instance
  overrides.
- **figo2cocos** — each frame → one self-contained `.prefab` (JSON array of
  `__id__`-linked engine objects) + shared `textures/` with `.meta`
  sprite-frames. Drop the output into a Cocos Creator 3.x project's `assets/`.
  Verified against Creator 3.8.
- **figo2unity** — each frame → one `.prefab` (multi-document YAML: GameObject
  → RectTransform/CanvasRenderer/`UI.Image`/`UI.Text`) + shared `textures/`
  with TextureImporter `.meta`. Drop anywhere under `Assets/` (needs
  `com.unity.ugui`). Verified against Unity 6000.0.
  **Color space**: output matches the design pixel-for-pixel in **Gamma**
  projects; for **Linear** projects re-export with `--linear` (pre-compensates
  translucent alpha to approximate the sRGB look).

`--fonts DIR` bundles real .ttf/.otf files as engine font assets and matches
each text node by (family, weight, italic). UUIDs/GUIDs are content-derived so
reruns are stable.

### web2canvas (`tools/web2canvas/`, Node)

Captures a rendered page into canvas.json using the installed Edge/Chrome
(playwright-core, no Chromium download); bundles react/react-dom/babel so
non-ESM React apps load offline.

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

### One command (html2godot)

```
node tools/web2canvas/html2godot.js <url|file.html> --out <godotDir> \
     [--states "a,b,c"] [--fonts DIR] [--root SEL] [--viewport WxH] [--wait MS] [--prefabs]
```

Runs web2canvas → figo2godot; `<godotDir>/` opens directly in Godot 4, with
intermediate artifacts in `<godotDir>/.web2canvas/`.

### Quick smoke test

```
build\figo2cocos.exe examples\html\starfall_menu.canvas.json out_cocos
```

`examples/html/starfall_menu.canvas.json` is a pre-captured 2-frame game UI
(main menu + settings, 15 textures) — no browser run needed.

## Claude Code skills

| Skill | What it does |
|---|---|
| `figo2cocos` | export a design to Cocos Creator 3.x prefabs, verify in-editor |
| `figo2unity` | export a design to Unity UGUI prefabs, verify in-editor |
| `web-to-godot` | React/HTML page → Godot 4 project, end-to-end with self-verify |

## Known limits

Only screen-level capture (`window.__nav`) — popups opened only by a click and
content scrolled beyond the viewport are not captured; letter-spacing in Godot
Labels; per-corner NinePatch radii use the max corner. Google Fonts are blocked
during capture — use `--fonts` with local faces.

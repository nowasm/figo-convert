# Figo Prefab Importer — Quick Start

Version 1.0.0

Turn a **Figma design (.fig)** or a **figo canvas.json** into ready-to-use
**UGUI prefabs** (.prefab + textures + fonts), entirely inside the Editor.
Windows and macOS Editors are supported.

## Convert your first design

1. Open **Tools → Figo Prefab Importer...**
2. Pick a design file:
   - a `.fig` saved from Figma via *File → Save local copy...*, or
   - a `canvas.json` (figo's intermediate format), or Figma REST JSON.
   - Try the bundled sample: `Samples/starfall_menu.canvas.json`.
3. Choose an output folder under `Assets/` and press **Convert**.
   On the very first conversion the plugin offers to download the small
   command-line converter (see *The converter binary* below) — confirm once.
4. Drag any generated prefab into a Canvas. Done.

You can also right-click a `.fig` / `.json` asset in the Project view →
**Figo → Convert to UGUI Prefabs**.

## What you get

- Each top-level design frame becomes one self-contained `.prefab`
  (GameObject → RectTransform / CanvasRenderer / `UI.Image` / `UI.Text`).
- A shared `textures/` folder of deduplicated PNG sprites. Solid rectangles
  become sprite-less tinted `UI.Image`s (zero textures); gradients, vectors,
  images, strokes and effects are baked pixel-faithfully.
- Responsive constraints from the design map to RectTransform anchors.
- Stable GUIDs: re-converting the same design produces clean diffs.

## Options

| Option | Meaning |
|---|---|
| Fonts folder | Folder with the design's .ttf/.otf files — bundled as font assets and matched per text by family/weight/italic. Without it, text uses the built-in font. |
| Frame filter | Convert only the named top-level frame. |
| Texture scale | Supersampling factor for baked sprites (2 = @2x). |
| Extract shared components | Detect repeated UI (cards, rows, buttons) and emit nested prefabs in `components/` with per-instance overrides (Unity 2022.2+). |
| Color space | **Gamma** output is pixel-identical to the design. In **Linear** projects translucent blending (glows, translucent panels) would render ~50% brighter, so the Linear mode pre-compensates alpha to approximate the sRGB look. *Auto* reads your project's Player Settings. |

## The converter binary

Conversion runs `figo2unity`, a self-contained command-line converter
(no other dependencies; your design never leaves your machine — the network
is used only to fetch the converter itself, once).

- On the first **Convert** the plugin asks to download the binary for your
  platform from the open-source figo repository and caches it in
  `Library/FigoPrefabImporter/`:
  - Windows x64 (~3 MB):
    <https://raw.githubusercontent.com/nowasm/figo/master/prebuild/win-x64/figo2unity.exe>
  - macOS universal, Intel + Apple Silicon, macOS 11+ (~10 MB):
    <https://raw.githubusercontent.com/nowasm/figo/master/prebuild/macos/figo2unity>
- The cache survives normal work; if `Library/` is deleted the binary is
  simply re-downloaded. To force a fresh copy, delete
  `Library/FigoPrefabImporter/` and convert again.
- **Offline / air-gapped machines**: download the file above on any machine
  and place it in the package's `Editor/Bin/` folder (create it next to
  `Editor/FigoPrefabImporter.cs`). A binary there always takes precedence
  over the downloaded cache.

## Requirements & notes

- Windows or macOS Editor, Unity 2022.3 or newer. The project needs
  `com.unity.ugui` (present in any UI project).
- Supported design features: solid/gradient/image fills, strokes, per-corner
  radii, masks, blend modes, shadows/blurs, auto-layout and constraints,
  component instances with overrides, multi-line rich text.

## Support

- Issues / questions: https://github.com/nowasm/figo/issues

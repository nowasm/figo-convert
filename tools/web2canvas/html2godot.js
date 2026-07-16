#!/usr/bin/env node
// html2godot — one command: a React/HTML page -> a Godot 4 project.
// Runs web2canvas (DOM -> canvas.json + images) then figo2godot (canvas.json
// -> .tscn + sprites + manifest), wiring the intermediate paths together.
//
//   node html2godot.js <url|file.html> --out <godotDir>
//        [--states "a,b,c"] [--flows FILE] [--manual] [--append] [--fonts DIR] [--root SEL]
//        [--viewport WxH] [--wait MS] [--browser msedge|chrome] [--figo2godot <exe>]
//        [--prefabs] [--prefab-anon]
//
// --states reaches screens behind a window.__nav hook; --flows captures
// click-driven popups/overlays (a JSON array of captures with interaction steps).
// --manual opens a headed window with an in-page toolbar — a human stages each
// screen and clicks 采集; --append continues the existing capture in --out's
// .web2canvas (so a manual session can top up an automated batch run).
//
// Result: <godotDir>/ is an openable Godot project — one .tscn per screen,
// deduped sprites, bundled fonts, manifest.json, project.godot.

const path = require('path');
const fs = require('fs');
const { execFileSync } = require('child_process');

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}
const input = process.argv[2];
const out = arg('--out', null);
if (!input || input.startsWith('-') || !out) {
  console.error('usage: html2godot <url|file.html> --out <godotDir> [--states ...] [--flows FILE] [--fonts DIR] [--root SEL] [--viewport WxH] [--wait MS] [--browser ...] [--prefabs] [--prefab-anon] [--ai-name] [--figo2godot <exe>]');
  process.exit(2);
}

const here = __dirname;
const inter = path.join(path.resolve(out), '.web2canvas');
fs.mkdirSync(inter, { recursive: true });
const canvas = path.join(inter, 'design.canvas.json');

// default figo2godot: the repo's build output (tools/web2canvas -> ../../build_godot)
let figo2godot = arg('--figo2godot', null);
if (!figo2godot) {
  for (const c of ['build_godot/figo2godot.exe', 'build/figo2godot.exe', 'build_godot/figo2godot']) {
    const p = path.resolve(here, '..', '..', c);
    if (fs.existsSync(p)) { figo2godot = p; break; }
  }
}
if (!figo2godot || !fs.existsSync(figo2godot)) {
  console.error('FAIL: figo2godot executable not found; pass --figo2godot <path>');
  process.exit(1);
}

const fonts = arg('--fonts', null);
const passthrough = [];
for (const k of ['--states', '--flows', '--root', '--viewport', '--wait', '--browser', '--scale', '--nav-fn', '--nav-reset', '--electron', '--electron-app']) {
  const v = arg(k, null);
  if (v != null) passthrough.push(k, v);
}
if (fonts) passthrough.push('--fonts', fonts);
if (process.argv.includes('--ai-name')) passthrough.push('--ai-name');  // vision-infer component names
if (process.argv.includes('--manual')) passthrough.push('--manual');    // human-driven capture (in-page toolbar)
if (process.argv.includes('--append')) passthrough.push('--append');    // continue an existing .web2canvas capture

console.log('=== web2canvas ===');
execFileSync(process.execPath, [path.join(here, 'index.js'), input, '-o', canvas, ...passthrough],
             { stdio: 'inherit' });

console.log('=== figo2godot ===');
const f2gArgs = [canvas, path.resolve(out)];
if (fonts) f2gArgs.push('--fonts', fonts);
if (process.argv.includes('--prefabs')) f2gArgs.push('--prefabs');
// hand-picked comps from a --manual session: always extract as prefabs
const pinsFile = canvas.replace(/\.canvas\.json$/, '.pins.json');
if (fs.existsSync(pinsFile)) {
  try {
    const pins = JSON.parse(fs.readFileSync(pinsFile, 'utf8'));
    if (Array.isArray(pins) && pins.length) f2gArgs.push('--prefab-pin', pins.join(','));
  } catch (e) { /* malformed sidecar: ignore */ }
}
if (process.argv.includes('--prefab-anon')) f2gArgs.push('--prefab-anon');  // also dedupe repeated ANONYMOUS containers (implies --prefabs)
const noPrefab = arg('--no-prefab', null);  // comma list of generic wrapper component types to inline
if (noPrefab) f2gArgs.push('--no-prefab', noPrefab);
execFileSync(figo2godot, f2gArgs, { stdio: 'inherit' });

console.log(`\nRESULT: OK -> ${path.resolve(out)} (open in Godot 4)`);

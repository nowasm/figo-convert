#!/usr/bin/env node
// Package the export wizard into a double-clickable app.
//
//   cd desktop && npm run pack       ->  dist/figo 导出向导-darwin-arm64/
//
// Payload layout inside the app (Contents/Resources on mac):
//   web2canvas/   index.js, html2godot.js, prototype.js, gui/, node_modules/
//   bin/          figo2godot, figoplay
// server.js finds the binaries via process.resourcesPath/bin; html2godot
// re-enters node via process.execPath + ELECTRON_RUN_AS_NODE.
'use strict';
const fs = require('fs');
const path = require('path');
const { packager } = require('@electron/packager');

const DESKTOP = __dirname;
const TOOL = path.resolve(DESKTOP, '..');
const REPO = path.resolve(TOOL, '..', '..');
const PAYLOAD = path.join(DESKTOP, 'payload');

function copy(src, dst) {
  fs.cpSync(src, dst, { recursive: true, dereference: true });
}

(async () => {
  fs.rmSync(PAYLOAD, { recursive: true, force: true });

  const w2c = path.join(PAYLOAD, 'web2canvas');
  fs.mkdirSync(w2c, { recursive: true });
  for (const f of ['index.js', 'html2godot.js', 'prototype.js', 'package.json'])
    copy(path.join(TOOL, f), path.join(w2c, f));
  copy(path.join(TOOL, 'gui'), path.join(w2c, 'gui'));
  copy(path.join(TOOL, 'node_modules'), path.join(w2c, 'node_modules'));

  const bin = path.join(PAYLOAD, 'bin');
  fs.mkdirSync(bin, { recursive: true });
  for (const b of ['figo2godot', 'figo2cocos', 'figo2unity', 'figoplay']) {
    const exe = process.platform === 'win32' ? b + '.exe' : b;
    const src = path.join(REPO, 'build', exe);
    if (!fs.existsSync(src)) throw new Error(`missing ${src} — build the repo first`);
    copy(src, path.join(bin, exe));
  }

  const out = await packager({
    dir: DESKTOP,
    out: path.join(DESKTOP, 'dist'),
    overwrite: true,
    ignore: [/^\/payload($|\/)/, /^\/dist($|\/)/, /^\/node_modules($|\/)/, /^\/pack\.js$/],
    extraResource: [w2c, bin],
    appBundleId: 'com.figo.export-wizard',
    name: 'figo-export-wizard',
    executableName: 'figo-export-wizard',
  });
  console.log('packaged:', out.join('\n'));
})().catch((e) => { console.error(e.message); process.exit(1); });

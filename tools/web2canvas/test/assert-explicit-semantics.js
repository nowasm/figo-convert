const fs = require('fs');

const document = JSON.parse(fs.readFileSync('test/out.canvas.json', 'utf8'));
const frame = document.document.children[0].children[0];
const nodes = [];

(function visit(node) {
  nodes.push(node);
  for (const child of node.children || []) visit(child);
})(frame);

const screen = nodes.find(node => node.name === 'SampleScreen');
if (!screen) throw new Error('data-name was not preserved for the screen root');

const button = nodes.find(node => node.name === 'SendButton');
if (!button) throw new Error('data-name was not preserved for the button');
if (button.comp !== 'ActionButton' || button.compRoot !== true) {
  throw new Error(`data-comp was not preserved: ${JSON.stringify(button)}`);
}

// The gradient card is nested below the screen root. A regression in raster
// isolation hid every ancestor with visibility:hidden, so Chromium emitted a
// fully transparent PNG even though the canvas manifest looked correct.
const nestedRaster = nodes.find(node =>
  node.name === 'card' &&
  (node.fillPaints || []).some(paint => paint.type === 'IMAGE'));
if (!nestedRaster) throw new Error('nested gradient raster was not collected');
const rasterRef = nestedRaster.fillPaints.find(paint => paint.type === 'IMAGE').image.filename;
const rasterPath = rasterRef.replace(/^images[\\/]/, 'test/images/');
if (!fs.existsSync(rasterPath) || fs.statSync(rasterPath).size < 4096) {
  throw new Error(`nested raster is missing or transparent: ${rasterPath}`);
}

console.log('explicit semantics OK');

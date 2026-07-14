#!/usr/bin/env node
// web2canvas GUI wizard — open a folder, see an analysis report, click export.
//
//   node gui/server.js [--port N] [--no-open]
//
// Wraps the existing CLI chain (html2godot.js -> figo2godot). "Analysis" IS a
// real conversion into a staging dir; the report reads the staging outputs
// (canvas.json comp groups, components/*.tscn, manifest.json, figoplay shot);
// "Export" finalizes staging into the chosen folder (re-running figo2godot
// only when the user unchecked prefabs -> --no-prefab).
'use strict';
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, execFile } = require('child_process');

const TOOL_DIR = path.resolve(__dirname, '..');
const REPO = path.resolve(TOOL_DIR, '..', '..');

function findBin(name) {
  const cands = [
    process.env[name.toUpperCase()],                     // FIGO2GODOT=/x/y
    // packaged desktop app: binaries live in <app>/Contents/Resources/bin
    process.resourcesPath && path.join(process.resourcesPath, 'bin', name),
    path.join(REPO, 'build', name),
    path.join(REPO, 'build', name + '.exe'),
    // runtime tools (figoplay/figoedit) build in the sibling figo repo
    path.join(REPO, '..', 'figo', 'build', name),
    path.join(REPO, '..', 'figo', 'build', name + '.exe'),
  ].filter(Boolean);
  for (const c of cands) if (fs.existsSync(c)) return c;
  return null;
}

const FIGO2GODOT = findBin('figo2godot');
const FIGOPLAY = findBin('figoplay');

// Per-engine exporters share one CLI shape: <canvas.json> <outDir> --prefabs
// [--no-prefab A,B]. Analysis always reports via the godot run in staging;
// cocos/unity re-export from the same canvas.json at export time.
const ENGINES = {
  godot: { bin: FIGO2GODOT, hint: '可直接用 Godot 4 打开' },
  cocos: { bin: findBin('figo2cocos'), hint: '把文件夹拖进 Cocos Creator 3.x 工程的 assets/ 即可' },
  unity: { bin: findBin('figo2unity'), hint: '把文件夹拖进 Unity 工程的 Assets/ 即可' },
};

// ---- single-job state (one wizard run at a time) ----
const job = {
  phase: 'idle',   // idle | analyzing | ready | exporting | done | error
  log: [],
  error: null,
  srcDir: null,
  staging: null,
  opts: null,      // {root, viewport, wait}
  report: null,
  exportedTo: null,
  exportHint: null,
};

function logLine(s) { job.log.push(s); }

function run(cmd, argv, opts = {}) {
  return new Promise((resolve, reject) => {
    logLine(`$ ${path.basename(cmd)} ${argv.join(' ')}`);
    // ELECTRON_RUN_AS_NODE: when hosted inside Electron, process.execPath is
    // the app binary — this env makes spawned children behave as plain node.
    const p = spawn(cmd, argv, { cwd: opts.cwd || TOOL_DIR,
                                 env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' } });
    let out = '';
    const onData = (d) => {
      out += d;
      for (const line of d.toString().split('\n')) if (line.trim()) logLine(line.trimEnd());
    };
    p.stdout.on('data', onData);
    p.stderr.on('data', onData);
    p.on('error', reject);
    p.on('close', (code) => code === 0 ? resolve(out) : reject(new Error(`${path.basename(cmd)} exited ${code}`)));
  });
}

// Capture runs in the user's installed Chrome/Edge (playwright channel).
function detectBrowser() {
  if (process.platform === 'darwin') {
    if (fs.existsSync('/Applications/Google Chrome.app')) return 'chrome';
    if (fs.existsSync('/Applications/Microsoft Edge.app')) return 'msedge';
  } else if (process.platform === 'win32') {
    for (const [p, ch] of [
      ['C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe', 'chrome'],
      ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe', 'msedge'],
    ]) if (fs.existsSync(p)) return ch;
  }
  return null;
}

// ---- input folder -> entry html ----
// Scan .html files (2 levels deep) and score which one is the real app: a
// root index.html is often just a catalog page while the actual React app
// lives in a subdir next to its .jsx modules (seen in real projects).
function scanFolder(dir) {
  const htmls = [];
  const walk = (d, depth) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      if (e.isDirectory()) {
        if (depth < 2 && !/^(node_modules|\.git|godot_ready|fonts|assets|images)$/i.test(e.name))
          walk(path.join(d, e.name), depth + 1);
      } else if (/\.html?$/i.test(e.name)) htmls.push(path.join(d, e.name));
    }
  };
  walk(dir, 0);
  const scored = htmls.map(f => {
    let score = 0;
    const src = fs.readFileSync(f, 'utf8').slice(0, 200000);
    const sibs = fs.readdirSync(path.dirname(f));
    const hasJsx = sibs.some(s => /\.(jsx|tsx)$/i.test(s));
    if (hasJsx) score += 2;   // React app dir
    if (/react(\.|-dom)/i.test(src)) score += 1;
    const stage = /id="stage"/.test(src);
    if (stage) score += 1;
    // suggest capture root + viewport from the stage element's CSS
    let root = null, viewport = null;
    if (stage) {
      root = '#stage';
      const m = /#stage\s*{[^}]*width:\s*(\d+)px[^}]*height:\s*(\d+)px/.exec(src);
      if (m) viewport = `${m[1]}x${m[2]}`;
    }
    // Babel-in-browser React needs a few seconds to compile before first paint
    return { file: path.relative(dir, f), score, root, viewport, wait: hasJsx ? 3000 : null };
  }).sort((a, b) => b.score - a.score || a.file.length - b.file.length);
  // project fonts: a fonts/fonts.css at the folder root (or next to the entry)
  let fonts = null;
  for (const c of [path.join(dir, 'fonts'), scored[0] && path.join(dir, path.dirname(scored[0].file), 'fonts')])
    if (c && fs.existsSync(path.join(c, 'fonts.css'))) { fonts = c; break; }
  // capture config: a *.flows.json next to the folder root or the entry lists
  // every screen/popup to capture (nav + click steps) — without it only the
  // page's default state is captured
  let flows = null, flowCount = 0;
  for (const d of [dir, scored[0] && path.join(dir, path.dirname(scored[0].file))]) {
    if (!d || !fs.existsSync(d)) continue;
    const f = fs.readdirSync(d).find(n => /\.flows\.json$/i.test(n));
    if (f) {
      try {
        const arr = JSON.parse(fs.readFileSync(path.join(d, f), 'utf8'));
        if (Array.isArray(arr)) { flows = path.join(d, f); flowCount = arr.length; break; }
      } catch (e) { /* malformed: ignore */ }
    }
  }
  return { htmls: scored, fonts, flows, flowCount };
}

function findEntry(dir, rel) {
  if (rel) {
    const f = path.normalize(path.join(dir, rel));
    return f.startsWith(path.normalize(dir)) && fs.existsSync(f) ? f : null;
  }
  const scan = scanFolder(dir);
  return scan.htmls.length ? path.join(dir, scan.htmls[0].file) : null;
}

// ---- report building from staging outputs ----
// Rects are FRAME-relative (fi = frame index): multi-screen docs lay frames
// side by side in document space, but the report shot only covers frame 0.
function walkComps(node, ox, oy, fi, groups) {
  const t = node.transform || {};
  const x = ox + (t.x || 0), y = oy + (t.y || 0);
  if (node.compRoot && node.comp) {
    const g = groups[node.comp] || (groups[node.comp] = { name: node.comp, count: 0, rects: [] });
    g.count++;
    g.rects.push({ x, y, w: node.size ? node.size.x : 0, h: node.size ? node.size.y : 0, fi });
  }
  for (const c of node.children || []) walkComps(c, x, y, fi, groups);
}

async function buildReport() {
  const st = job.staging;
  const canvasPath = path.join(st, '.web2canvas', 'design.canvas.json');
  const doc = JSON.parse(fs.readFileSync(canvasPath, 'utf8')).document;
  // document -> CANVAS page(s) -> top-level FRAMEs (one per captured screen)
  const frames = [];
  for (const page of doc.children || [])
    for (const f of page.children || [])
      if (f.type === 'FRAME') frames.push({ name: f.name, w: f.size ? f.size.x : 0, h: f.size ? f.size.y : 0 });

  // component groups straight from the capture (candidates), extraction status
  // from what figo2godot actually wrote to components/
  const groups = {};
  {
    let fi = 0;
    for (const page of doc.children || [])
      for (const f of page.children || [])
        if (f.type === 'FRAME') {
          for (const c of f.children || []) walkComps(c, 0, 0, fi, groups);
          fi++;
        }
  }
  const compDir = path.join(st, 'components');
  const extracted = new Set(fs.existsSync(compDir)
    ? fs.readdirSync(compDir).filter(f => f.endsWith('.tscn')).map(f => f.replace(/\.tscn$/, ''))
    : []);
  const prefabs = [];
  for (const name of Object.keys(groups).sort()) {
    const g = groups[name];
    prefabs.push({
      // thumbnail crop needs a frame-0 instance (the shot covers frame 0 only)
      name, count: g.count, rect: g.rects.find(r => r.fi === 0) || null, rects: g.rects,
      screens: new Set(g.rects.map(r => r.fi)).size,
      extracted: extracted.has(name),
      reason: extracted.has(name) ? null
        : g.count < 2 ? '仅出现 1 次'
        : '结构过简或变体差异大（已内联）',
    });
  }
  // anon/extra .tscn not matching a named group (e.g. --prefab-anon output)
  for (const name of extracted) if (!groups[name]) prefabs.push({ name, count: 0, rect: null, rects: [], extracted: true, reason: null });

  const manifest = JSON.parse(fs.readFileSync(path.join(st, 'manifest.json'), 'utf8'));
  const sprites = Object.values(manifest.sprites || {}).map(s => s.file);

  // ground-truth render via figoplay (validates the conversion + emits
  // diagnostics); figoplay resolves app.json paths against its cwd, so run it
  // inside .web2canvas with relative paths. Falls back to the browser
  // screenshot web2canvas already saved (design.web.png).
  const w2c = path.join(st, '.web2canvas');
  let shot = null, shotScale = 1, warnings = [];
  if (FIGOPLAY && frames.length) {
    fs.writeFileSync(path.join(w2c, 'noop.js'), '// gui report shot\n');
    // Project fonts for the ground-truth render. figoplay scans its fonts dir
    // non-recursively, while projects nest faces in per-family subdirs — so
    // flatten via symlinks into the staging area.
    let fontsRel = null;
    if (job.opts.fonts) {
      const flat = path.join(w2c, 'fonts');
      fs.mkdirSync(flat, { recursive: true });
      const link = (d) => {
        for (const e of fs.readdirSync(d, { withFileTypes: true })) {
          const p = path.join(d, e.name);
          if (e.isDirectory()) link(p);
          else if (/\.(ttf|otf)$/i.test(e.name)) {
            try { fs.symlinkSync(p, path.join(flat, e.name)); } catch (e) { /* dup name: first wins */ }
          }
        }
      };
      link(job.opts.fonts);
      if (fs.readdirSync(flat).length) fontsRel = 'fonts';
    }
    fs.writeFileSync(path.join(w2c, 'app.json'), JSON.stringify({
      design: 'design.canvas.json', script: 'noop.js',
      viewport: [Math.round(frames[0].w), Math.round(frames[0].h)],
      ...(fontsRel ? { fonts: fontsRel } : {}),
    }));
    try {
      await run(FIGOPLAY, ['.', '--shot', 'report_shot.png', '--frames', '2'], { cwd: w2c });
    } catch (e) { logLine(`figoplay shot failed: ${e.message}`); }
  }
  for (const cand of ['report_shot.png', 'design.web.png']) {
    const f = path.join(w2c, cand);
    if (!fs.existsSync(f)) continue;
    shot = '/files/.web2canvas/' + cand;
    // PNG header: width is the big-endian u32 at byte 16
    shotScale = fs.readFileSync(f).readUInt32BE(16) / frames[0].w;
    break;
  }
  const diagPath = path.join(w2c, 'report_shot.diagnostics.json');
  if (fs.existsSync(diagPath)) {
    const diag = JSON.parse(fs.readFileSync(diagPath, 'utf8'));
    const all = (Array.isArray(diag) ? diag : diag.warnings || []).map(w =>
      typeof w === 'string' ? w : `${w.type || 'warning'}: ${w.node || ''} ${w.detail || w.message || ''}`.trim());
    // per-node repeats of the same message drown the report — aggregate
    const agg = new Map();
    for (const w of all) agg.set(w, (agg.get(w) || 0) + 1);
    warnings = [...agg].map(([m, c]) => c > 1 ? `${m}（×${c}）` : m);
  }

  if (!Object.keys(groups).length && !job.opts.anon)
    warnings.unshift('页面里没有发现组件标识（入口不是 React 应用，或选错了入口页）。' +
                     '可换一个入口，或在高级选项勾选「按结构重复识别预制体」后重新分析。');

  job.report = {
    srcDir: job.srcDir,
    frames, prefabs, sprites, shot, shotScale, warnings,
    fonts: fs.existsSync(path.join(st, 'fonts')) ? fs.readdirSync(path.join(st, 'fonts')) : [],
    suggestedOutBase: path.join(path.dirname(job.srcDir), path.basename(job.srcDir)),
    engines: Object.fromEntries(Object.entries(ENGINES).map(([k, v]) => [k, !!v.bin])),
  };
}

// ---- actions ----
async function analyze(dir, opts) {
  job.phase = 'analyzing';
  job.log = [];
  job.error = null;
  job.report = null;
  job.srcDir = dir;
  job.opts = opts;
  job.staging = fs.mkdtempSync(path.join(os.tmpdir(), 'w2c-gui-'));
  const entry = findEntry(dir, opts.entry);
  if (!entry) throw new Error('文件夹里没找到 .html 入口文件');
  if (opts.fonts == null) opts.fonts = scanFolder(dir).fonts;  // auto-wire project fonts
  if (!FIGO2GODOT) throw new Error('找不到 figo2godot，可在仓库 build/ 下构建，或设 FIGO2GODOT 环境变量');
  const argv = [path.join(TOOL_DIR, 'html2godot.js'), entry,
                '--out', job.staging, '--prefabs', '--figo2godot', FIGO2GODOT];
  if (electronCapture) {
    // desktop app: capture inside our own bundled Chromium — no system browser
    argv.push('--electron', electronCapture.exe);
    if (electronCapture.appDir) argv.push('--electron-app', electronCapture.appDir);
  } else {
    const browser = detectBrowser();
    if (!browser) throw new Error('没有找到 Chrome 或 Edge 浏览器——页面采集需要它，请先安装 Google Chrome');
    argv.push('--browser', browser);
  }
  if (opts.root) argv.push('--root', opts.root);
  if (opts.viewport) argv.push('--viewport', opts.viewport);
  if (opts.wait) argv.push('--wait', String(opts.wait));
  if (opts.flows) argv.push('--flows', opts.flows);
  else if (opts.states) argv.push('--states', opts.states);
  if (opts.anon) argv.push('--prefab-anon');   // plain-HTML pages: dedupe repeated anonymous structures
  if (opts.fonts) argv.push('--fonts', opts.fonts);
  await run(process.execPath, argv);
  await buildReport();
  job.phase = 'ready';
}

function copyOutput(from, to) {
  fs.mkdirSync(to, { recursive: true });
  fs.cpSync(from, to, { recursive: true, filter: (src) => !path.basename(src).startsWith('.web2canvas') });
}

async function doExport(outDir, exclude, engine = 'godot') {
  job.phase = 'exporting';
  const eng = ENGINES[engine];
  if (!eng) throw new Error(`未知引擎: ${engine}`);
  if (!eng.bin) throw new Error(`找不到 figo2${engine}，请先在仓库 build/ 下构建`);
  let from = job.staging;
  if (engine !== 'godot' || exclude.length) {
    // re-export from the staged canvas.json (the capture is reused); godot
    // with no exclusions skips this and copies the staging output directly
    const redo = fs.mkdtempSync(path.join(os.tmpdir(), 'w2c-gui-x-'));
    const argv = [path.join(job.staging, '.web2canvas', 'design.canvas.json'), redo, '--prefabs'];
    if (exclude.length) argv.push('--no-prefab', exclude.join(','));
    await run(eng.bin, argv);
    from = redo;
  }
  copyOutput(from, outDir);
  job.exportedTo = outDir;
  job.exportHint = eng.hint;
  job.phase = 'done';
  const reveal = engine === 'godot' ? path.join(outDir, 'project.godot') : outDir;
  if (process.platform === 'darwin') execFile('open', ['-R', reveal], () => {});
}

let customPicker = null;      // the Electron shell injects a native dialog here
let electronCapture = null;   // {exe, appDir?} — capture in the shell's own Chromium

function pickFolder(prompt) {
  if (customPicker) return customPicker(prompt);
  return new Promise((resolve) => {
    if (process.platform !== 'darwin') return resolve(null);
    execFile('osascript', ['-e', `POSIX path of (choose folder with prompt "${prompt}")`],
      (err, stdout) => resolve(err ? null : stdout.trim()));
  });
}

// ---- http ----
const MIME = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css',
               '.png': 'image/png', '.json': 'application/json', '.svg': 'image/svg+xml' };

function sendJson(res, obj, code = 200) {
  res.writeHead(code, { 'content-type': 'application/json' });
  res.end(JSON.stringify(obj));
}

function body(req) {
  return new Promise((resolve) => {
    let b = '';
    req.on('data', d => b += d);
    req.on('end', () => { try { resolve(JSON.parse(b || '{}')); } catch (e) { resolve({}); } });
  });
}

const server = http.createServer(async (req, res) => {
  const url = decodeURIComponent((req.url || '/').split('?')[0]);
  try {
    if (url === '/' || url === '/index.html') {
      res.writeHead(200, { 'content-type': 'text/html' });
      return res.end(fs.readFileSync(path.join(__dirname, 'app.html')));
    }
    if (url === '/api/status') {
      return sendJson(res, { phase: job.phase, log: job.log, error: job.error,
                             report: job.report, exportedTo: job.exportedTo,
                             exportHint: job.exportHint || null });
    }
    if (url === '/api/pick' && req.method === 'POST') {
      const { prompt } = await body(req);
      const p = await pickFolder(prompt || '选择要转换的网页文件夹');
      return sendJson(res, { path: p });
    }
    if (url === '/api/scan' && req.method === 'POST') {
      const { dir } = await body(req);
      if (!dir || !fs.existsSync(dir) || !fs.statSync(dir).isDirectory())
        return sendJson(res, { error: '不是有效的文件夹路径' }, 400);
      return sendJson(res, scanFolder(path.resolve(dir)));
    }
    if (url === '/api/analyze' && req.method === 'POST') {
      const { dir, entry, root, viewport, wait, states, anon, flows } = await body(req);
      if (!dir || !fs.existsSync(dir) || !fs.statSync(dir).isDirectory())
        return sendJson(res, { error: '不是有效的文件夹路径' }, 400);
      if (job.phase === 'analyzing' || job.phase === 'exporting')
        return sendJson(res, { error: '已有任务在进行中' }, 409);
      analyze(path.resolve(dir), { entry, root, viewport, wait, states, anon: !!anon, flows })
        .catch(e => { job.phase = 'error'; job.error = e.message; logLine('ERROR: ' + e.message); });
      return sendJson(res, { ok: true });
    }
    if (url === '/api/export' && req.method === 'POST') {
      const { outDir, exclude, engine } = await body(req);
      if (job.phase !== 'ready' && job.phase !== 'done')
        return sendJson(res, { error: '还没有可导出的分析结果' }, 409);
      if (!outDir) return sendJson(res, { error: '请填写导出目录' }, 400);
      doExport(path.resolve(outDir), exclude || [], engine || 'godot')
        .catch(e => { job.phase = 'error'; job.error = e.message; logLine('ERROR: ' + e.message); });
      return sendJson(res, { ok: true });
    }
    if (url.startsWith('/files/')) {
      if (!job.staging) { res.writeHead(404); return res.end(); }
      const f = path.normalize(path.join(job.staging, url.slice(7)));
      if (!f.startsWith(job.staging)) { res.writeHead(403); return res.end(); }
      let data;
      try { data = fs.readFileSync(f); } catch (e) { res.writeHead(404); return res.end(); }
      res.writeHead(200, { 'content-type': MIME[path.extname(f)] || 'application/octet-stream' });
      return res.end(data);
    }
    res.writeHead(404);
    res.end('not found');
  } catch (e) {
    if (!res.headersSent) sendJson(res, { error: e.message }, 500);
    else res.end();
  }
});

// Start listening; resolves with the actual URL (port 0 = OS-assigned).
// Used both by the CLI below and by the Electron shell (desktop/main.js).
function start({ port = 9333, pickFolder: picker = null, electronCapture: cap = null } = {}) {
  if (picker) customPicker = picker;
  if (cap) electronCapture = cap;
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => {
      const url = `http://127.0.0.1:${server.address().port}/`;
      if (!FIGO2GODOT) console.log('warning: figo2godot not found — build it or set FIGO2GODOT');
      resolve(url);
    });
  });
}

module.exports = { start };

if (require.main === module) {
  let args = { port: 9333, open: true };
  for (let i = 2; i < process.argv.length; i++) {
    const t = process.argv[i];
    if (t === '--port') args.port = +process.argv[++i];
    else if (t === '--no-open') args.open = false;
  }
  start({ port: args.port }).then((url) => {
    console.log(`web2canvas GUI: ${url}`);
    if (args.open && process.platform === 'darwin') execFile('open', [url], () => {});
  }).catch((e) => { console.error(e.message); process.exit(1); });
}

// Electron shell for the web2canvas export wizard: boots the existing gui
// server in-process, opens a window on it. All conversion work stays in
// gui/server.js — this file is only window + native-dialog glue.
'use strict';
const { app, BrowserWindow, dialog } = require('electron');
const path = require('path');

// Capture mode (--w2c-capture): this same binary doubles as the capture
// browser — index.js drives it via playwright _electron, so the packaged app
// needs no system Chrome. Just open a bare window at the requested viewport;
// everything else (goto, inject, screenshot) comes over the wire.
if (process.argv.includes('--w2c-capture')) {
  const val = (k, d) => { const i = process.argv.indexOf(k); return i >= 0 ? process.argv[i + 1] : d; };
  app.commandLine.appendSwitch('force-device-scale-factor', val('--w2c-scale', '1'));
  app.whenReady().then(() => {
    const win = new BrowserWindow({
      width: +val('--w2c-vw', '1280'), height: +val('--w2c-vh', '720'),
      useContentSize: true, title: '正在采集页面…', resizable: false,
    });
    win.removeMenu?.();
    win.loadURL('about:blank');
  });
  app.on('window-all-closed', () => app.quit());
  return;
}

// Packaged: resources/web2canvas/gui/server.js (extraResources); dev: ../gui.
function serverPath() {
  const packaged = path.join(process.resourcesPath || '', 'web2canvas', 'gui', 'server.js');
  return app.isPackaged ? packaged : path.join(__dirname, '..', 'gui', 'server.js');
}

app.whenReady().then(async () => {
  const win = new BrowserWindow({
    width: 1100, height: 860,
    title: 'figo 导出向导',
    backgroundColor: '#0d1017',
    webPreferences: { contextIsolation: true },
  });
  win.removeMenu?.();

  const { start } = require(serverPath());
  try {
    const url = await start({
      port: 0,  // OS-assigned; no clash with a dev server on 9333
      // capture runs in this very binary (--w2c-capture branch above); a bare
      // dev electron needs the app dir, the packaged app is its own entry
      electronCapture: { exe: process.execPath, appDir: app.isPackaged ? null : __dirname },
      pickFolder: async (prompt) => {
        const r = await dialog.showOpenDialog(win, {
          title: prompt, properties: ['openDirectory'],
        });
        return r.canceled ? null : r.filePaths[0];
      },
    });
    await win.loadURL(url);
  } catch (e) {
    dialog.showErrorBox('启动失败', e.message);
    app.quit();
  }
});

app.on('window-all-closed', () => app.quit());

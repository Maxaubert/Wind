// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Wind for Mac: the host is a WKWebView (Sources/Wind/SettingsWeb.swift). Page -> host through
// window.webkit.messageHandlers.wind; host -> page through window.__windReceive(msg). Without the host
// (Playwright, vite dev) messages go to window.__windMock and replies come back the same way.
const mac = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wind;
const listeners = new Set();
window.__windReceive = msg => listeners.forEach(fn => fn(msg));

export function onMessage(fn) { listeners.add(fn); return () => listeners.delete(fn); }
export function post(msg) {
  if (mac) mac.postMessage(msg);
  else if (window.__windMock) window.__windMock(msg);
}
// getSession() resolves { values, runningModel }: values is config.json as strings (bools "1"/"0",
// numbers as text, bind lists comma-joined); runningModel the engine Wind runs now ("draw" or "zoom").
// The first call after load is answered from window.__windInit (injected by the host before the page
// runs); it is consumed once, so later calls (and a reload) always ask the host for fresh state.
export function getSession() {
  const init = window.__windInit;
  if (init) {
    delete window.__windInit;
    if (init.mac) window.__windInitMac = init.mac;   // getMacStatus answers its first call from it
    return Promise.resolve({ values: init.values || {}, runningModel: init.runningModel || '' });
  }
  return new Promise(resolve => {
    const off = onMessage(m => {
      if (m && m.type === 'config') { off(); resolve({ values: m.values || {}, runningModel: m.runningModel || '' }); }
    });
    post({ type: 'getConfig' });
  });
}
// Writes one config.json key; the host types it from Config, clamps it and saves (applies live).
// A refused key or value comes back as {type:'configWriteFailed', key}.
export function setConfig(key, value) { post({ type: 'setConfig', key, value: String(value) }); }
// Launch mode: the host loads ...?mode=onboard for the first-run setup.
export function getMode() {
  return new URLSearchParams(location.search).get('mode') === 'onboard' ? 'onboard' : 'settings';
}
// Window actions the page asks for: close, minimize, quitWind (Cmd+Q).
export function windowControl(action) { post({ type: 'window', action }); }
// "Edit settings file" -> the host opens config.json in the default editor.
export function openIni() { post({ type: 'openIni' }); }
// "Export diagnostics" -> the host zips the logs and config.json to the Desktop and shows it in Finder.
export function exportDiagnostics() { post({ type: 'exportDiagnostics' }); }

// Mac: permissions, macOS Zoom shortcuts, login item, log level ({accessibility, screenRecording,
// zoomShortcuts[], loginItem, logLevel}). The host pushes macStatus when it changes.
export function getMacStatus() {
  const init = (window.__windInit && window.__windInit.mac) || window.__windInitMac;
  delete window.__windInitMac;
  return new Promise(resolve => {
    const off = onMessage(m => { if (m && m.type === 'macStatus') { off(); resolve(m); } });
    post({ type: 'getMacStatus' });
    if (init) { off(); resolve({ type: 'macStatus', ...init }); }
  });
}
export function onMacStatus(fn) { return onMessage(m => { if (m && m.type === 'macStatus') fn(m); }); }
// openAccessibility | openScreenRecording | fixZoomShortcuts | setLogin (value "1"/"0") | showLog | rerunSetup
export function macAction(action, value = '') { post({ type: 'macAction', action, value: String(value) }); }
// While a keybind box records, Wind's own hotkeys are paused so the press reaches the page.
/** The top of the page column holds only the banner (scrolled to the top): the host drags the window there. */
export function titleBand(on) { post({ type: 'titleBand', on: on ? '1' : '0' }); }
export function captureBinds(on) { post({ type: 'captureBinds', on: on ? '1' : '0' }); }

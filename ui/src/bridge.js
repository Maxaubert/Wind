const wv = window.chrome && window.chrome.webview;
const listeners = new Set();
if (wv) wv.addEventListener('message', e => listeners.forEach(fn => fn(e.data)));

export function onMessage(fn) { listeners.add(fn); return () => listeners.delete(fn); }
export function post(msg) {
  if (wv) wv.postMessage(msg);
  else if (window.__windMock) window.__windMock(msg);
}
export function getConfig() {
  return new Promise(resolve => {
    const off = onMessage(m => { if (m && m.type === 'config') { off(); resolve(m.values || {}); } });
    post({ type: 'getConfig' });
  });
}
export function setConfig(key, value) { post({ type: 'setConfig', key, value: String(value) }); }
// Launch mode: WindConfig.exe navigates to ...?mode=onboard for first-launch setup.
export function getMode() {
  return new URLSearchParams(location.search).get('mode') === 'onboard' ? 'onboard' : 'settings';
}
// Custom title bar buttons -> host runs ShowWindow(SW_MINIMIZE) / WM_CLOSE.
export function windowControl(action) { post({ type: 'window', action }); }
// "Edit config file" -> host opens magnifier.ini with the registered .ini handler (Notepad fallback).
export function openIni() { post({ type: 'openIni' }); }
// "Export diagnostics" -> host zips %LOCALAPPDATA%\Wind\logs to the Desktop and reveals it.
export function exportDiagnostics() { post({ type: 'exportDiagnostics' }); }

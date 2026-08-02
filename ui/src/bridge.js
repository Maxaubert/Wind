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
// force:true is the "Discard" answer to the unsaved-changes guard: close without re-asking.
export function windowControl(action, force = false) {
  post({ type: 'window', action, force: force ? '1' : '0' });
}
// Mirror the staged/unsaved state to the host so its WM_CLOSE can put up the guard for Alt+F4 and
// the system menu too, not just our own title-bar button.
export function setDirty(v) { post({ type: 'dirty', value: v ? '1' : '0' }); }
// MPO (Multi-Plane Overlay) lives in HKLM, so reading is free but writing needs elevation.
// getMpoState is a plain read; setMpoDisabled raises a UAC prompt in the host and resolves with the
// RE-READ state, so a cancelled prompt reverts the row rather than showing a change that never was.
export function getMpoState() {
  return new Promise(resolve => {
    const off = onMessage(m => { if (m && m.type === 'mpoState') { off(); resolve(!!m.disabled); } });
    post({ type: 'mpoState' });
  });
}
export function setMpoDisabled(disabled) {
  return new Promise(resolve => {
    const off = onMessage(m => {
      if (m && m.type === 'mpoApplied') { off(); resolve({ ok: !!m.ok, disabled: !!m.disabled }); }
    });
    post({ type: 'setMpoDisabled', value: disabled ? '1' : '0' });
  });
}
// Offered only after an MPO change lands: DWM reads OverlayTestMode at boot.
export function rebootNow() { post({ type: 'rebootNow' }); }
// "Edit config file" -> host opens magnifier.ini with the registered .ini handler (Notepad fallback).
export function openIni() { post({ type: 'openIni' }); }
// "Export diagnostics" -> host zips %LOCALAPPDATA%\Wind\logs to the Desktop and reveals it.
export function exportDiagnostics() { post({ type: 'exportDiagnostics' }); }
// "+" on an app list -> host shows a file picker and replies with the bare exe NAME (not a path:
// the core matches on file name only). Resolves to '' when the user cancels.
export function pickExe() {
  return new Promise(resolve => {
    const off = onMessage(m => { if (m && m.type === 'exePicked') { off(); resolve(m.name || ''); } });
    post({ type: 'pickExe' });
  });
}

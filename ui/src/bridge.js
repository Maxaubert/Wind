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

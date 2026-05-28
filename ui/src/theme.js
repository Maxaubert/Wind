import { setConfig } from './bridge.js';

// uiTheme = 'auto' | 'dark' | 'light'. auto follows prefers-color-scheme; dark/light force a class
// that overrides the media query. Persisted in magnifier.ini (UI-only key; the core ignores it).
export function applyTheme(mode) {
  const c = document.documentElement.classList;
  c.remove('force-dark', 'force-light');
  if (mode === 'dark') c.add('force-dark');
  else if (mode === 'light') c.add('force-light');
}
export function currentTheme(values) {
  return values && values.uiTheme ? values.uiTheme : 'auto';
}
// The sun/moon toggle cycles auto -> dark -> light -> auto and persists.
export function nextTheme(mode) {
  return mode === 'auto' ? 'dark' : mode === 'dark' ? 'light' : 'auto';
}
export function setTheme(mode) { applyTheme(mode); setConfig('uiTheme', mode); }

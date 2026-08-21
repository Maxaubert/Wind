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
// The sun/moon toggle flips the EFFECTIVE theme: auto resolves against the system preference
// first, so one click always visibly changes something. (The old auto -> dark -> light cycle
// needed TWO clicks to reach light on a dark system: auto -> dark was invisible.)
export function nextTheme(mode, systemDark = typeof window !== 'undefined' &&
    window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches) {
  const dark = mode === 'dark' || (mode === 'auto' && systemDark);
  return dark ? 'light' : 'dark';
}
export function setTheme(mode) { applyTheme(mode); setConfig('uiTheme', mode); }

// Pure helpers for the session model (spec 2026-10-01): the live ini is the session, the active
// profile file is the saved state. No bridge, no DOM, so Playwright can import it directly.
import { groups, groupRows, bindKeys } from './settings-schema.js';

// Mirrors IsGlobalProfileKey in src/profiles.cpp: these are never part of a profile, so they never
// count as an unsaved change. The five tray keys (#313) live in the live ini only.
export const GLOBAL_KEYS = new Set(['profile', 'onboarded', 'uiTheme', 'uiPalette', 'showAdvanced',
  'trayPerf', 'traySliders', 'traySliderOrder', 'trayToggles', 'trayToggleOrder']);

// Must match the core's shipped defaults (src/config.h): every keybind ships unbound except Quick
// zoom. Seeding anything else here would invent a binding the user never chose.
export const KB_DEFAULTS = {
  zoomInButton: '0', zoomInVk: '0', zoomOutButton: '0', zoomOutVk: '0',
  zoomInButton2: '0', zoomOutButton2: '0', zoomInVk2: '0', zoomOutVk2: '0',
  zoomInMods: '0', zoomOutMods: '0', zoomInMods2: '0', zoomOutMods2: '0',
  hideCursorVk: '0', hideCursorMods: '0', quickZoomVk: '112', quickZoomMods: '0',
  panLeftVk: '0', panLeftMods: '0', panRightVk: '0', panRightMods: '0',
  panUpVk: '0', panUpMods: '0', panDownVk: '0', panDownMods: '0',
  cursorLockMods: '0', recenterMods: '0',
};

// Defaults for every key the page shows. Applied to BOTH sides of the comparison, so a key the file
// does not mention reads the same as the default and never counts as a change.
export function schemaDefaults() {
  const d = { ...KB_DEFAULTS };
  for (const r of groups.flatMap(groupRows)) {
    if (r.key[0] !== '_' && r.def !== undefined) d[r.key] = r.def;
    for (const k of bindKeys(r)) if (!(k in d)) d[k] = '0';
  }
  return d;
}

// session: { values, saved } as the host reports them. Returns both maps with defaults filled in.
export function fill(session) {
  const d = schemaDefaults();
  return { values: { ...d, ...(session.values || {}) }, saved: { ...d, ...(session.saved || session.values || {}) } };
}

// Keys whose live value differs from the saved profile (global keys ignored, values compared as text).
export function changedKeys(values, saved) {
  const out = [];
  for (const k of Object.keys(values)) {
    if (GLOBAL_KEYS.has(k)) continue;
    if (String(values[k] ?? '').trim() !== String(saved[k] ?? '').trim()) out.push(k);
  }
  return out;
}

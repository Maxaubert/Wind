// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Wind for Mac: pure model of the keybind boxes. No DOM globals, no bridge, so node and Playwright can
// import it directly.
//
// A keybind row's value is values[row.key], a comma list of Mac bind texts ("mouse4,ctrl+opt+equal";
// '' = unbound). The page shows one box per bind; at most `row.max` (default 1) can be ADDED, but a
// longer list written by hand still shows in full. Captures write the canonical text (formatBind).
// The pan row writes four lists (row.panKeys) in one patch; the wheel row (row.wheelMods) holds
// modifiers only ("ctrl", "ctrl+opt"; '' = off).
import { parseBind, formatBind, bindVerdict, splitBinds } from './keybindRules.js';
import { CODE_TO_KEY, MODIFIER_CODES, bindParts, bindLabel, bindSpoken } from './bindNames.js';

export { splitBinds };
export const maxOf = (row) => row.max || 1;

// [{ idx, text, bind, ok, mods, main, label, spoken }]: `ok` is false for text Wind ignores (it cannot
// read it, or the rules refuse it); such a box shows the raw text so it can be seen and removed.
export function unitsOf(row, values) {
  return splitBinds(values[row.key]).map((text, idx) => {
    const bind = parseBind(text);
    const usable = !!bind && (bind.key !== undefined || bind.mouse !== undefined);
    if (!usable) return { idx, text, bind, ok: false, mods: '', main: text, label: text, spoken: text };
    const p = bindParts(bind);
    return { idx, text, bind, ok: bindVerdict(bind) === 'ok', mods: p.mods, main: p.main, label: bindLabel(bind), spoken: bindSpoken(bind) };
  });
}

// The patch that removes box `idx`.
export function removePatch(row, values, idx) {
  return { [row.key]: splitBinds(values[row.key]).filter((_, i) => i !== idx).join(',') };
}
// The patch that stores `text` in box `idx`, or appends it when idx is null. A bind already in the
// list is not stored twice.
export function placePatch(row, values, idx, text) {
  const list = splitBinds(values[row.key]);
  if (idx === null || idx === undefined || idx >= list.length) list.push(text);
  else list[idx] = text;
  const out = [];
  for (const t of list) if (!out.includes(t)) out.push(t);
  return { [row.key]: out.join(',') };
}

// --- Events -> binds (plain objects, so tests can pass fakes) ---------------------------------------
// DOM modifier flags -> Mac modifier names (Option arrives as altKey, Command as metaKey).
export function eventMods(e) {
  const m = [];
  if (e.ctrlKey) m.push('ctrl');
  if (e.altKey) m.push('opt');
  if (e.shiftKey) m.push('shift');
  if (e.metaKey) m.push('cmd');
  return m;
}
// A keydown -> { kind: 'mod' } (a modifier key alone: wait for the main key) | { kind: 'key', bind } |
// { kind: 'unknown' } (a key Binds.swift has no name for) | null (nothing usable: IME, synthesized).
export function keyEventBind(e) {
  if (!e.code || e.code === 'Unidentified') return null;
  if (MODIFIER_CODES.has(e.code)) return { kind: 'mod' };
  const key = CODE_TO_KEY[e.code];
  if (!key) return { kind: 'unknown' };
  return { kind: 'key', bind: { mods: eventMods(e), key } };
}
// DOM mouse buttons 0 left, 1 middle, 2 right, 3 back, 4 forward -> CGEvent numbers 0, 2, 1, 3, 4.
export const DOM_TO_MOUSE = { 0: 0, 1: 2, 2: 1, 3: 3, 4: 4 };
export function mouseEventBind(e) {
  const n = DOM_TO_MOUSE[e.button];
  return n === undefined ? null : { mods: eventMods(e), mouse: n };
}
export { formatBind, bindLabel, bindSpoken, bindVerdict };

// --- Pan: one shared modifier set on the four arrow keys ------------------------------------------
export const PAN_KEYS = ['panLeft', 'panUp', 'panRight', 'panDown'];
export const PAN_ARROWS = ['left', 'up', 'right', 'down'];
const panKeysOf = (row) => (row && row.panKeys) || PAN_KEYS;
const arrowOf = (k) => PAN_ARROWS[PAN_KEYS.indexOf(k)];
// { kind: 'none' } | { kind: 'arrows', mods } | { kind: 'custom' } (anything else: more binds, other keys
// or mixed modifiers, from the config file).
export function panState(row, values) {
  const keys = panKeysOf(row);
  const lists = keys.map((k) => splitBinds(values[k]));
  if (lists.every((l) => !l.length)) return { kind: 'none', mods: [] };
  let mods = null;
  for (let i = 0; i < keys.length; i++) {
    if (lists[i].length !== 1) return { kind: 'custom', mods: [] };
    const b = parseBind(lists[i][0]);
    if (!b || b.key !== arrowOf(keys[i]) || !b.mods.length) return { kind: 'custom', mods: [] };
    if (mods && mods.join('+') !== b.mods.join('+')) return { kind: 'custom', mods: [] };
    mods = b.mods;
  }
  return { kind: 'arrows', mods };
}
// mods [] clears all four (pan keys off).
export function panPatch(row, mods) {
  const p = {};
  for (const k of panKeysOf(row)) p[k] = mods.length ? formatBind({ mods, key: arrowOf(k) }) : '';
  return p;
}
export const panBinds = (mods) => PAN_ARROWS.map((key) => ({ mods, key }));

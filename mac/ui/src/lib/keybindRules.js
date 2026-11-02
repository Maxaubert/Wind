// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Wind for Mac: mirror of Sources/WindCore/Binds.swift (parseBind, formatBind, bindVerdict). Both are
// tested against Tests/fixtures/mac_keybind_cases.txt (ui/tests/keybind-rules.nodetest.mjs and
// wind-checks), so a rule changed in one place fails the other's tests until it is changed there too.
import { MODS, MOD_ALIAS, KEY_NAMES, KEY_ALIAS } from './bindNames.js';

// Text -> { mods: ['ctrl', ...], key } | { mods, mouse } | { mods } (modifier-only), or null when
// Binds.swift parseBind cannot read it.
export function parseBind(text) {
  text = String(text ?? '');
  if (text.endsWith('++')) return parseBind(text.slice(0, -2) + '+plus');
  const parts = text.toLowerCase().split('+').map((p) => p.replace(/^[ \t]+|[ \t]+$/g, ''));
  if (!parts.length || parts.some((p) => !p)) return null;
  const mods = new Set();
  let key, mouse;
  for (let i = 0; i < parts.length; i++) {
    const p = parts[i];
    if (MOD_ALIAS[p]) { mods.add(MOD_ALIAS[p]); continue; }
    if (i !== parts.length - 1) return null;
    const m = /^mouse(\d+)$/.exec(p);
    const n = m ? Number(m[1]) : NaN;
    if (m && n >= 2 && n <= 31) mouse = n;
    else if (KEY_NAMES.has(p) || KEY_ALIAS[p]) key = KEY_ALIAS[p] || p;
    else return null;
  }
  if (key === undefined && mouse === undefined && !mods.size) return null;
  const b = { mods: MODS.filter((x) => mods.has(x)) };
  if (key !== undefined) b.key = key;
  if (mouse !== undefined) b.mouse = mouse;
  return b;
}

// Canonical text, as Binds.swift formatBind writes it: "ctrl+opt+equal", "shift+mouse3", "ctrl".
export function formatBind(b) {
  const parts = MODS.filter((m) => b.mods.includes(m));
  if (b.key !== undefined) parts.push(b.key);
  else if (b.mouse !== undefined) parts.push('mouse' + b.mouse);
  return parts.join('+');
}

const F_KEYS = new Set(Array.from({ length: 20 }, (_, i) => 'f' + (i + 1)));
const MODIFIER_KEYS = new Set(['capslock']);   // Binds.swift modifierKeyCodes, of the names it reads
// Keys that type a character with opt held (Binds.swift charKeyCodes).
const CHAR_KEYS = new Set([...'abcdefghijklmnopqrstuvwxyz0123456789',
  'equal', 'minus', 'leftbracket', 'rightbracket', 'quote', 'semicolon', 'backslash', 'comma', 'slash', 'period',
  'grave', 'space', ...Array.from({ length: 10 }, (_, i) => 'keypad' + i), 'keypaddecimal', 'keypadmultiply',
  'keypadplus', 'keypadminus', 'keypaddivide', 'keypadequals']);

// Binds.swift bindVerdict: 'ok' | 'empty' | 'mousebutton' | 'modifier' | 'needsmod' | 'cmdalone' | 'opttypes'.
export function bindVerdict(b) {
  const has = (m) => b.mods.includes(m);
  if (b.mouse !== undefined) return b.mouse < 2 ? 'mousebutton' : 'ok';
  if (b.key === undefined) return b.mods.length ? 'ok' : 'empty';
  if (MODIFIER_KEYS.has(b.key)) return 'modifier';
  if (F_KEYS.has(b.key)) return 'ok';
  const ctrl = has('ctrl'), opt = has('opt'), cmd = has('cmd');
  if (!ctrl && !opt && !cmd) return 'needsmod';
  if (b.mods.length === 1 && cmd && b.key !== 'escape') return 'cmdalone';   // Cmd+Esc: no app uses it (quit preset)
  if (opt && !ctrl && !cmd && CHAR_KEYS.has(b.key)) return 'opttypes';
  return 'ok';
}
// Text -> verdict, with 'invalid' for text parseBind cannot read (the fixture's form).
export function checkBindText(text) {
  const b = parseBind(text);
  return b ? bindVerdict(b) : 'invalid';
}

// The bind lists Config.binds() reads, with their labels. quit is exempt from the rules there.
export const BIND_KEYS = [
  ['zoomIn', 'Zoom in'], ['zoomOut', 'Zoom out'], ['quickZoom', 'Quick zoom'],
  ['panLeft', 'Pan left'], ['panRight', 'Pan right'], ['panUp', 'Pan up'], ['panDown', 'Pan down'],
  ['inspect', 'Inspect mode'], ['hideCursor', 'Hide pointer'],
];
export const splitBinds = (v) => String(v ?? '').split(',').map((s) => s.trim()).filter(Boolean);

// Stored binds Wind ignores (unreadable text, a modifier-only action bind, or a refused bind).
// Returns [{ label, key, keys: [key], binds: ['opt+a'], patch: { [key]: '<the list without them>' } }];
// the caller applies `patch` (an empty string clears the list). wheelZoomMods is checked too: it must
// be modifiers only.
export function droppedBinds(cfg) {
  const out = [];
  for (const [key, label] of BIND_KEYS) {
    const list = splitBinds(cfg[key]);
    const bad = list.filter((t) => { const b = parseBind(t); return !b || (b.key === undefined && b.mouse === undefined) || bindVerdict(b) !== 'ok'; });
    if (bad.length) out.push({ label, key, keys: [key], binds: bad, patch: { [key]: list.filter((t) => !bad.includes(t)).join(',') } });
  }
  const w = String(cfg.wheelZoomMods ?? '');
  if (w) {
    const b = parseBind(w);
    if (!b || b.key !== undefined || b.mouse !== undefined)
      out.push({ label: 'Zoom with the scroll wheel', key: 'wheelZoomMods', keys: ['wheelZoomMods'], binds: [w], patch: { wheelZoomMods: '' } });
  }
  return out;
}

// Why a press was refused, in plain words (shown under the box and spoken). `what` is the readable
// combination ("⌥A", "Left click").
export function refusalText(verdict, what) {
  switch (verdict) {
    case 'mousebutton': return `${what} can't be a binding. Use another mouse button or a key.`;
    case 'modifier':    return `${what} can't be the key of a binding. Hold the modifiers and press another key.`;
    case 'needsmod':    return `${what} alone would stop you typing or using it. Add ⌃ Control, ⌥ Option or ⌘ Command.`;
    case 'cmdalone':    return `${what} is used by app shortcuts. Add ⌃ Control or ⌥ Option.`;
    case 'opttypes':    return `${what} types a character on macOS. Add ⌃ Control or ⌘ Command.`;
    case 'empty':       return 'Hold at least one modifier key, such as ⌃ Control.';
    case 'invalid':     return `${what || 'That key'} can't be a keybind on the Mac.`;
    default:            return '';
  }
}

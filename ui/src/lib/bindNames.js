// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Wind for Mac: the names of binds, shared by the Settings keybind boxes and the onboarding capture.
// Key names are the ones Sources/WindCore/Binds.swift reads (its keyCodes table); modifiers show as
// the macOS glyphs ⌃ ⌥ ⇧ ⌘ in that order, the order formatBind writes them.

// Modifier names in config order, their glyphs, their spoken words, and the parse aliases.
export const MODS = ['ctrl', 'opt', 'shift', 'cmd'];
export const MOD_GLYPH = { ctrl: '⌃', opt: '⌥', shift: '⇧', cmd: '⌘' };
export const MOD_WORD = { ctrl: 'Control', opt: 'Option', shift: 'Shift', cmd: 'Command' };
export const MOD_ALIAS = { shift: 'shift', ctrl: 'ctrl', control: 'ctrl', opt: 'opt', option: 'opt', alt: 'opt', cmd: 'cmd', command: 'cmd' };

// Every canonical key name in Binds.swift keyCodes, with its label and its spoken name.
const KEYS = {
  return: ['↩', 'Return'], tab: ['⇥', 'Tab'], space: ['Space', 'Space'], delete: ['⌫', 'Delete'],
  escape: ['Esc', 'Escape'], capslock: ['Caps Lock', 'Caps Lock'], forwarddelete: ['⌦', 'Forward Delete'],
  help: ['Help', 'Help'], home: ['Home', 'Home'], end: ['End', 'End'], pageup: ['Page Up', 'Page Up'],
  pagedown: ['Page Down', 'Page Down'], left: ['←', 'Left Arrow'], right: ['→', 'Right Arrow'],
  up: ['↑', 'Up Arrow'], down: ['↓', 'Down Arrow'],
  equal: ['=', 'Equals'], minus: ['-', 'Minus'], leftbracket: ['[', 'Left Bracket'], rightbracket: [']', 'Right Bracket'],
  quote: ["'", 'Quote'], semicolon: [';', 'Semicolon'], backslash: ['\\', 'Backslash'], comma: [',', 'Comma'],
  slash: ['/', 'Slash'], period: ['.', 'Period'], grave: ['`', 'Grave Accent'],
  keypaddecimal: ['Keypad .', 'Keypad Decimal'], keypadmultiply: ['Keypad *', 'Keypad Multiply'],
  keypadplus: ['Keypad +', 'Keypad Plus'], keypadminus: ['Keypad -', 'Keypad Minus'],
  keypaddivide: ['Keypad /', 'Keypad Divide'], keypadequals: ['Keypad =', 'Keypad Equals'],
  keypadclear: ['Clear', 'Keypad Clear'], keypadenter: ['Keypad Enter', 'Keypad Enter'],
};
for (const c of 'abcdefghijklmnopqrstuvwxyz') KEYS[c] = [c.toUpperCase(), c.toUpperCase()];
for (let d = 0; d <= 9; d++) { KEYS[String(d)] = [String(d), String(d)]; KEYS['keypad' + d] = ['Keypad ' + d, 'Keypad ' + d]; }
for (let f = 1; f <= 20; f++) KEYS['f' + f] = ['F' + f, 'F' + f];
export const KEY_NAMES = new Set(Object.keys(KEYS));
// Parse-only aliases (Binds.swift keyCodes: esc, plus, =, -).
export const KEY_ALIAS = { esc: 'escape', plus: 'equal', '=': 'equal', '-': 'minus' };

// DOM KeyboardEvent.code -> Binds.swift key name. Codes not listed cannot be bound on the Mac.
export const CODE_TO_KEY = {
  Equal: 'equal', Minus: 'minus', BracketLeft: 'leftbracket', BracketRight: 'rightbracket', Quote: 'quote',
  Semicolon: 'semicolon', Backslash: 'backslash', Comma: 'comma', Slash: 'slash', Period: 'period',
  Backquote: 'grave', Enter: 'return', Tab: 'tab', Space: 'space', Backspace: 'delete', Escape: 'escape',
  CapsLock: 'capslock', Delete: 'forwarddelete', Help: 'help', Insert: 'help', Home: 'home', End: 'end',
  PageUp: 'pageup', PageDown: 'pagedown', ArrowLeft: 'left', ArrowRight: 'right', ArrowUp: 'up', ArrowDown: 'down',
  NumpadDecimal: 'keypaddecimal', NumpadMultiply: 'keypadmultiply', NumpadAdd: 'keypadplus',
  NumpadSubtract: 'keypadminus', NumpadDivide: 'keypaddivide', NumpadEqual: 'keypadequals',
  NumpadEnter: 'keypadenter', NumLock: 'keypadclear', NumpadClear: 'keypadclear',
};
for (const c of 'ABCDEFGHIJKLMNOPQRSTUVWXYZ') CODE_TO_KEY['Key' + c] = c.toLowerCase();
for (let d = 0; d <= 9; d++) { CODE_TO_KEY['Digit' + d] = String(d); CODE_TO_KEY['Numpad' + d] = 'keypad' + d; }
for (let f = 1; f <= 20; f++) CODE_TO_KEY['F' + f] = 'f' + f;
// The modifier keys themselves: a press of one alone waits for the main key.
export const MODIFIER_CODES = new Set(['ShiftLeft', 'ShiftRight', 'ControlLeft', 'ControlRight', 'AltLeft', 'AltRight',
  'MetaLeft', 'MetaRight', 'OSLeft', 'OSRight', 'Fn', 'FnLock']);

export const keyLabel = (k) => (KEYS[k] ? KEYS[k][0] : k);
export const keySpoken = (k) => (KEYS[k] ? KEYS[k][1] : k);
// CGEvent button numbers: 2 middle, 3 back, 4 forward. The label keeps the config's number.
export const mouseLabel = (n) => (n === 2 ? 'Middle click' : n === 0 ? 'Left click' : n === 1 ? 'Right click' : 'Mouse ' + n);
export const mouseSpoken = (n) => (n === 3 ? 'Mouse 3, back' : n === 4 ? 'Mouse 4, forward' : mouseLabel(n));

// mods: ['ctrl', 'opt'] -> '⌃⌥' (always in config order).
export const modsGlyphs = (mods) => MODS.filter((m) => mods.includes(m)).map((m) => MOD_GLYPH[m]).join('');
export const modsSpoken = (mods) => MODS.filter((m) => mods.includes(m)).map((m) => MOD_WORD[m]).join(' ');

// A parsed bind ({ mods, key } | { mods, mouse } | { mods }) -> its parts for display:
// { mods: '⌃⌥', main: '=' }, and the whole as one string ('⌃⌥=') or spoken ('Control Option Equals').
export function bindParts(b) {
  const main = b.key ? keyLabel(b.key) : b.mouse !== undefined ? mouseLabel(b.mouse) : '';
  return { mods: modsGlyphs(b.mods), main };
}
export function bindLabel(b) {
  const p = bindParts(b);
  // A word after the glyphs reads better with a gap: "⌃⌥F18" stays tight, "⇧ Mouse 3" does not.
  return p.mods && p.main.length > 3 && !b.key ? p.mods + ' ' + p.main : p.mods + p.main;
}
export function bindSpoken(b) {
  const main = b.key ? keySpoken(b.key) : b.mouse !== undefined ? mouseSpoken(b.mouse) : '';
  return [modsSpoken(b.mods), main].filter(Boolean).join(' ');
}

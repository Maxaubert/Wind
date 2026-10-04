// Pure model of the hotkeys page (#318): how a keybind row's ini slots read as "bindings" and how a
// captured press is written back. No DOM, no bridge, so Playwright can import it directly.
//
// A row has one or two SLOTS (zoom rows two, the rest one). A slot holds a mouse button or wheel code
// (+ its modifiers) and/or a key (+ its modifiers); the core OR-combines them. The page shows each
// held part as its own binding box ("unit"): at most `row.max` (default 1) can be ADDED, and a slot
// the page writes holds exactly one part, so an old ini that kept a button and a key in one slot
// still shows both, and re-recording one keeps the other.
import { modList, vkName, shortButtonName, popcount } from './bindNames.js';

export { popcount };
export const maxOf = (row) => row.max || 1;

const num = (values, k) => (k ? Number(values[k] || 0) : 0);
// The ini keys of each slot, from the schema row.
function slotsOf(row) {
  const out = [{ btn: row.buttonKey, bm: row.buttonModsKey, vk: row.vkKey, mods: row.modsKey }];
  if (row.vkKey2 || row.buttonKey2) out.push({ btn: row.buttonKey2, bm: row.buttonModsKey2, vk: row.vkKey2, mods: row.modsKey2 });
  return out;
}

// [{ slot, part: 'btn' | 'key', caps: ['Ctrl', 'Wheel up'] }] in slot order, the button part first.
export function unitsOf(row, values) {
  const out = [];
  slotsOf(row).forEach((s, slot) => {
    const b = num(values, s.btn);
    if (b) out.push({ slot, part: 'btn', caps: [...modList(num(values, s.bm)), shortButtonName(b)] });
    const vk = num(values, s.vk);
    if (vk) out.push({ slot, part: 'key', caps: [...modList(num(values, s.mods)), vkName(vk)] });
  });
  return out;
}

// The patch that empties one unit.
export function clearUnit(row, unit) {
  const s = slotsOf(row)[unit.slot];
  const keys = unit.part === 'btn' ? [s.btn, s.bm] : [s.vk, s.mods];
  return Object.fromEntries(keys.filter(Boolean).map((k) => [k, '0']));
}

// The patch that stores a captured press, replacing `unit` (or adding one when unit is null).
// captured: { kind: 'key', vk, mods } | { kind: 'btn', code, mods }. It lands in the first slot that is
// completely empty once `unit` is gone, preferring the unit's own slot, so every written slot holds
// one part; with no empty slot it overwrites the unit's slot.
export function placeBinding(row, values, unit, captured) {
  const slots = slotsOf(row);
  const gone = unit ? clearUnit(row, unit) : {};
  const eff = { ...values, ...gone };
  const empty = (s) => !num(eff, s.btn) && !num(eff, s.vk);
  const order = slots.map((_, i) => i);
  if (unit) order.sort((a, b) => (b === unit.slot ? 1 : 0) - (a === unit.slot ? 1 : 0));
  const idx = order.find((i) => empty(slots[i]));
  const at = idx !== undefined ? idx : (unit ? unit.slot : -1);
  if (at < 0) return gone;
  const s = slots[at];
  const mine = captured.kind === 'btn'
    ? { [s.btn]: String(captured.code), ...(s.bm ? { [s.bm]: String(captured.mods) } : {}) }
    : { [s.vk]: String(captured.vk), ...(s.mods ? { [s.mods]: String(captured.mods) } : {}) };
  return { ...gone, ...mine };
}

// Pan: one shared modifier mask on the four arrow keys (the arrows themselves are fixed).
export const PAN_ARROWS = [37, 38, 39, 40];
const PAN_VK = ['panLeftVk', 'panUpVk', 'panRightVk', 'panDownVk'];
const PAN_MODS = ['panLeftMods', 'panUpMods', 'panRightMods', 'panDownMods'];
// { kind: 'none' } | { kind: 'arrows', mods } | { kind: 'custom' } (older keys that are not the arrows).
export function panState(values) {
  const n = (k) => Number(values[k] || 0);
  if (PAN_VK.every((k) => n(k) === 0)) return { kind: 'none', mods: 0 };
  const mods = n(PAN_MODS[0]);
  const arrows = PAN_VK.every((k, i) => n(k) === PAN_ARROWS[i]) && PAN_MODS.every((k) => n(k) === mods);
  return arrows && mods ? { kind: 'arrows', mods } : { kind: 'custom', mods: 0 };
}
export function panPatch(mods) {
  const p = {};
  PAN_VK.forEach((k, i) => { p[k] = mods ? String(PAN_ARROWS[i]) : '0'; });
  PAN_MODS.forEach((k) => { p[k] = String(mods); });
  return p;
}

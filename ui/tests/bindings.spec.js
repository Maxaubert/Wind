// The pure binding model of the Hotkeys page (#318): how slots read as boxes and where a capture lands.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { groups, groupRows } from '../src/settings-schema.js';
import { unitsOf, clearUnit, placeBinding, maxOf, panState, panPatch } from '../src/lib/bindings.js';
import { vkName, shortButtonName, modList, popcount } from '../src/lib/bindNames.js';
import { checkClickBind, refusalText } from '../src/lib/keybindRules.js';

const row = (k) => groupRows(groups.find((g) => g.id === 'hotkeys')).find((r) => r.key === k);
const zin = row('__zoomIn');

test('names: modifiers, keys and the short button names', () => {
  expect(modList(5)).toEqual(['Ctrl', 'Shift']);
  expect(popcount(15)).toBe(4);
  expect(vkName(33)).toBe('PageUp');
  expect(vkName(112)).toBe('F1');
  expect(shortButtonName(2)).toBe('Mouse 5');
  expect(shortButtonName(6)).toBe('Wheel up');
  expect(shortButtonName(7)).toBe('Wheel down');
});

test('limits: zoom rows take two bindings, every other row one', () => {
  expect(maxOf(row('__zoomIn'))).toBe(2);
  expect(maxOf(row('__zoomOut'))).toBe(2);
  for (const k of ['__pan', '__hideCursor', '__cursorLock']) expect(maxOf(row(k)), k).toBe(1);
});

test('units: each held part of a slot is one box, button part first, in slot order', () => {
  expect(unitsOf(zin, {})).toEqual([]);
  const v = { zoomInButton: '2', zoomInVk: '33', zoomInButton2: '6', zoomInButton2Mods: '1' };
  expect(unitsOf(zin, v).map((u) => [u.slot, u.part, u.caps.join('+')])).toEqual([
    [0, 'btn', 'Mouse 5'], [0, 'key', 'PageUp'], [1, 'btn', 'Ctrl+Wheel up']]);
  expect(unitsOf(row('__cursorLock'), { cursorLockVk: '113', cursorLockMods: '3' }).map((u) => u.caps)).toEqual([['Ctrl', 'Alt', 'F2']]);
});

test('clearUnit empties just that part', () => {
  expect(clearUnit(zin, { slot: 0, part: 'btn' })).toEqual({ zoomInButton: '0', zoomInButtonMods: '0' });
  expect(clearUnit(zin, { slot: 1, part: 'key' })).toEqual({ zoomInVk2: '0', zoomInMods2: '0' });
  expect(clearUnit(row('__hideCursor'), { slot: 0, part: 'key' })).toEqual({ hideCursorVk: '0', hideCursorMods: '0' });
});

test('placeBinding: an add lands in the first empty slot, one part per slot', () => {
  const key = { kind: 'key', vk: 116, mods: 2 };
  expect(placeBinding(zin, {}, null, key)).toEqual({ zoomInVk: '116', zoomInMods: '2' });
  expect(placeBinding(zin, { zoomInVk: '33' }, null, { kind: 'btn', code: 6, mods: 1 }))
    .toEqual({ zoomInButton2: '6', zoomInButton2Mods: '1' });
  expect(placeBinding(row('__hideCursor'), {}, null, key)).toEqual({ hideCursorVk: '116', hideCursorMods: '2' });
});

test('placeBinding: re-recording keeps the box in its own slot, or moves it when the slot holds another part', () => {
  const only = { zoomInVk: '33' };
  expect(placeBinding(zin, only, { slot: 0, part: 'key' }, { kind: 'btn', code: 7, mods: 1 }))
    .toEqual({ zoomInVk: '0', zoomInMods: '0', zoomInButton: '7', zoomInButtonMods: '1' });
  // Slot 0 holds a button AND a key (an older ini): re-recording the key with a wheel cannot share the
  // slot's button part, so it goes to the empty second slot and the button box is untouched.
  const both = { zoomInButton: '2', zoomInVk: '33' };
  expect(placeBinding(zin, both, { slot: 0, part: 'key' }, { kind: 'btn', code: 6, mods: 2 }))
    .toEqual({ zoomInVk: '0', zoomInMods: '0', zoomInButton2: '6', zoomInButton2Mods: '2' });
  // No empty slot at all: it overwrites the unit's own slot.
  const full = { zoomInButton: '2', zoomInVk: '33', zoomInButton2: '1', zoomInVk2: '34' };
  expect(placeBinding(zin, full, { slot: 0, part: 'key' }, { kind: 'key', vk: 116, mods: 0 }))
    .toEqual({ zoomInVk: '116', zoomInMods: '0' });
});

test('wheel codes follow the wheel rule: a modifier is required, Shift alone is not enough', () => {
  expect(checkClickBind(6, 1)).toBe('ok');
  expect(checkClickBind(7, 2)).toBe('ok');
  expect(checkClickBind(6, 0)).toBe('needsmod');
  expect(checkClickBind(7, 4)).toBe('shiftalone');
  expect(checkClickBind(8, 1)).toBe('never');
  expect(refusalText('twomods', 'Ctrl+Alt+Shift+F5')).toContain('more than two modifiers');
});

test('pan: none, arrows with one shared mask, custom for anything else', () => {
  expect(panState({}).kind).toBe('none');
  expect(panState(panPatch(3))).toEqual({ kind: 'arrows', mods: 3 });
  expect(panState({ ...panPatch(3), panLeftVk: '33' }).kind).toBe('custom');
  expect(panState({ ...panPatch(3), panUpMods: '1' }).kind).toBe('custom');
  expect(Object.keys(panPatch(0))).toHaveLength(8);
  expect(Object.values(panPatch(0)).every((v) => v === '0')).toBe(true);
});

test('hotkeys files contain no em-dash', () => {
  for (const f of ['../src/lib/bindings.js', '../src/lib/bindNames.js', '../src/controls/Bindings.svelte',
    '../src/controls/PanBinding.svelte', '../src/controls/bindings.css'])
    expect(readFileSync(new URL(f, import.meta.url), 'utf8')).not.toContain(String.fromCharCode(0x2014));
});

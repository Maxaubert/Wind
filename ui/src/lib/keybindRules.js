// Mirror of src/keybind_rules.h (issue #285). Both are tested against tests/fixtures/keybind_cases.txt
// (ui/tests/keybind-rules.spec.js and tests/test_keybind_rules.cpp), so a rule changed in one place
// fails the other's tests until it is changed there too.
export const MOD = { ctrl: 1, alt: 2, shift: 4, win: 8 };

const isModifierVk = vk => vk === 0x10 || vk === 0x11 || vk === 0x12 || (vk >= 0xA0 && vk <= 0xA5) || vk === 0x5B || vk === 0x5C;
const isTypingVk = vk => (vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) || vk === 0x20 ||
  (vk >= 0xBA && vk <= 0xC2) || (vk >= 0xDB && vk <= 0xDF) || vk === 0xE1 || vk === 0xE2;
const allowedAlone = vk => (vk >= 0x21 && vk <= 0x28) || vk === 0x2D || vk === 0x2E || (vk >= 0x70 && vk <= 0x87) ||
  vk === 0x13 || vk === 0x91 || (vk >= 0x60 && vk <= 0x6F);

export function checkKeyBind(vk, mods) {
  if (vk === 0) return 'ok';
  if (vk <= 0 || vk > 255) return 'never';
  if ([0x01, 0x02, 0x04, 0x05, 0x06, 0x08].includes(vk)) return 'never';
  if (isModifierVk(vk)) return 'modifier';
  mods &= 15;
  if (mods === 0) return allowedAlone(vk) ? 'ok' : 'notalone';
  const ctrl = !!(mods & 1), alt = !!(mods & 2), win = !!(mods & 8);
  if (mods === MOD.shift && isTypingVk(vk)) return 'shifttypes';
  if (ctrl && alt && !win && isTypingVk(vk)) return 'altgr';
  if (alt && !ctrl && !win && [0x73, 0x09, 0x1B, 0x20].includes(vk)) return 'system';
  if (ctrl && !alt && !win && vk === 0x1B) return 'system';
  if (ctrl && alt && (vk === 0x2E || vk === 0x6E)) return 'system';
  if (ctrl && alt && !win && vk === 0x09) return 'system';
  if (win && ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) || vk === 0x09 || vk === 0x20 ||
      (vk >= 0x25 && vk <= 0x28) || [0xBB, 0xBD, 0xBC, 0xBE, 0x6B, 0x6D, 0x13, 0x2C, 0x1B, 0x0D, 0x24, 0x70].includes(vk)))
    return 'windows';
  return 'ok';
}
export function checkWheelBind(mods) {
  mods &= 15;
  if (mods === 0) return 'needsmod';
  if (mods === MOD.shift) return 'shiftalone';   // Ctrl+wheel is allowed (#295): Wind eats the notch
  return 'ok';
}
export function checkClickBind(button, mods) {
  if (button === 0 || button === 1 || button === 2) return 'ok';
  if (button < 0 || button > 5) return 'never';
  mods &= 15;
  if (mods === 0) return 'needsmod';
  if (mods === MOD.ctrl) return 'ctrlalone';
  if (mods === MOD.shift) return 'shiftalone';
  return 'ok';
}

// Stored binds the rules refuse (#285). The core already reads each as unbound, so an old ini whose
// Inspect key was a bare letter would otherwise just stop working with no word why. Returns
// [{ label, keys }]: the row label and the ini keys to reset to 0.
const KEY_SLOTS = [
  ['Zoom in', 'zoomInVk', 'zoomInMods'], ['Zoom in (alternate)', 'zoomInVk2', 'zoomInMods2'],
  ['Zoom out', 'zoomOutVk', 'zoomOutMods'], ['Zoom out (alternate)', 'zoomOutVk2', 'zoomOutMods2'],
  ['Hide cursor', 'hideCursorVk', 'hideCursorMods'], ['Inspect mode', 'cursorLockVk', 'cursorLockMods'],
  ['Recenter', 'recenterVk', 'recenterMods'], ['Quick zoom', 'quickZoomVk', 'quickZoomMods'],
  ['Pan left', 'panLeftVk', 'panLeftMods'], ['Pan right', 'panRightVk', 'panRightMods'],
  ['Pan up', 'panUpVk', 'panUpMods'], ['Pan down', 'panDownVk', 'panDownMods'],
];
const BUTTON_SLOTS = [
  ['Zoom in', 'zoomInButton', 'zoomInButtonMods'], ['Zoom in (alternate)', 'zoomInButton2', 'zoomInButton2Mods'],
  ['Zoom out', 'zoomOutButton', 'zoomOutButtonMods'], ['Zoom out (alternate)', 'zoomOutButton2', 'zoomOutButton2Mods'],
];
export function droppedBinds(cfg) {
  const n = k => Number(cfg[k] || 0);
  const out = [];
  for (const [label, vk, mods] of KEY_SLOTS)
    if (n(vk) && checkKeyBind(n(vk), mods ? n(mods) : 0) !== 'ok') out.push({ label, keys: mods ? [vk, mods] : [vk] });
  for (const [label, b, mods] of BUTTON_SLOTS)
    if (n(b) && checkClickBind(n(b), n(mods)) !== 'ok') out.push({ label, keys: [b, mods] });
  if (n('zoomWheelMods') && checkWheelBind(n('zoomWheelMods')) !== 'ok')
    out.push({ label: 'Zoom with the scroll wheel', keys: ['zoomWheelMods'] });
  return out;
}

// Why a press was refused, in plain words (shown under the keycap and spoken). `what` is the
// readable combo ("Ctrl+Alt+2", "Ctrl+Left click", "A").
export function refusalText(verdict, what) {
  switch (verdict) {
    case 'never':      return `${what} can't be a keybind.`;
    case 'modifier':   return 'Hold the modifier and press another key.';
    case 'notalone':   return `${what} alone would stop you typing or using it. Add Ctrl, Alt or Win.`;
    case 'shifttypes': return `${what} types a character. Add Ctrl, Alt or Win.`;
    case 'altgr':      return `${what} is AltGr on many keyboards and types a character. Pick another key.`;
    case 'system':     return `${what} is reserved by Windows.`;
    case 'windows':    return `${what} is a Windows shortcut. Pick another key.`;
    case 'needsmod':   return `${what} needs a modifier: hold Alt, Win or Ctrl+Alt while you do it.`;
    case 'ctrlalone':  return `${what} is used by apps (zoom, select). Add Alt, Shift or Win.`;
    case 'shiftalone': return `${what} is used by apps (scroll, select). Add Ctrl, Alt or Win.`;
    default:           return '';
  }
}

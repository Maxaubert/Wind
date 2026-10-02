// Readable names for bindings, shared by the Settings hotkeys page and the onboarding capture.
export const MOD_BITS = [{ bit: 1, name: 'Ctrl' }, { bit: 2, name: 'Alt' }, { bit: 4, name: 'Shift' }, { bit: 8, name: 'Win' }];
export const modList = (m) => MOD_BITS.filter((b) => m & b.bit).map((b) => b.name);
export const popcount = (m) => modList(m).length;

const SPECIAL = {
  8: 'Backspace', 9: 'Tab', 13: 'Enter', 27: 'Esc', 32: 'Space',
  33: 'PageUp', 34: 'PageDown', 35: 'End', 36: 'Home',
  37: 'Left', 38: 'Up', 39: 'Right', 40: 'Down',
  45: 'Insert', 46: 'Delete', 91: 'LWin', 92: 'RWin',
  96: 'Num0', 97: 'Num1', 98: 'Num2', 99: 'Num3', 100: 'Num4',
  101: 'Num5', 102: 'Num6', 103: 'Num7', 104: 'Num8', 105: 'Num9',
  106: 'Num*', 107: 'Num+', 109: 'Num-', 110: 'Num.', 111: 'Num/',
  144: 'NumLock', 145: 'ScrollLock',
  186: ';', 187: '=', 188: ',', 189: '-', 190: '.', 191: '/', 192: '`',
  219: '[', 220: '\\', 221: ']', 222: "'",
};
// VK -> readable name. Covers the common cases; falls back to "Key N" for the rest.
export function vkName(vk) {
  if (SPECIAL[vk]) return SPECIAL[vk];
  if (vk >= 112 && vk <= 123) return 'F' + (vk - 111);
  if ((vk >= 48 && vk <= 57) || (vk >= 65 && vk <= 90)) return String.fromCharCode(vk);
  return 'Key ' + vk;
}

// Button ids as stored in the zoom slots: 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward), 3 left,
// 4 right, 5 middle, 6 wheel up, 7 wheel down (#318).
export const BUTTON_NAMES = { 1: 'Mouse button 4', 2: 'Mouse button 5', 3: 'Left click', 4: 'Right click', 5: 'Middle click',
                              6: 'Wheel up', 7: 'Wheel down' };
const SHORT_BUTTON = { 1: 'Mouse 4', 2: 'Mouse 5' };
export const shortButtonName = (btn) => SHORT_BUTTON[btn] || BUTTON_NAMES[btn] || ('Button ' + btn);
export const WHEEL_UP = 6, WHEEL_DOWN = 7;

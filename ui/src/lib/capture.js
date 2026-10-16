// Keybind captures that are listening right now (Bindings, PanBinding). Settings' page shortcuts
// (Ctrl+F, Ctrl+Q, Escape) stand down while one is: its window listener runs before theirs, so the
// combination being recorded would also run the shortcut (#495).
const listening = new Set();
export function setCapturing(id, on) { if (on) listening.add(id); else listening.delete(id); }
export const capturing = () => listening.size > 0;

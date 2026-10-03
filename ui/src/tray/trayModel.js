// Tray menu lists (pure, no DOM): which quick controls the tray flyout shows and in what order.
// Mirrors src/tray_items.cpp (ParseTrayLayout / WriteTrayLayout) so the page and WindTray read the
// same five global ini keys the same way: trayPerf, traySliders, traySliderOrder, trayToggles,
// trayToggleOrder. Items and limits: docs/superpowers/specs/2026-10-01-tray-flyout-design.md.

export const MAX_SLIDERS = 4;

// `icon` names live in design/icons.js. Order here is the default order.
export const SLIDERS = [
  { key: 'colorWarmPct', icon: 'warm', name: 'Warmth', desc: 'Screen warmth.' },
  { key: 'colorDimPct', icon: 'bright', name: 'Brightness', desc: 'Screen brightness.' },
  { key: 'maxLevel', icon: 'maxz', name: 'Max zoom', desc: 'The highest magnification.' },
  { key: 'zoomInSpeed', icon: 'zin', name: 'Zoom-in speed', desc: 'How quickly the view magnifies.' },
  { key: 'zoomOutSpeed', icon: 'zout', name: 'Zoom-out speed', desc: 'How quickly the view returns.' },
  { key: 'panSpeed', icon: 'pan', name: 'Arrow key speed', desc: 'How fast the view pans.' },
  { key: 'cursorSmoothing', icon: 'smooth', name: 'Pan smoothing', desc: 'Gentle inertia while panning.' },
  { key: 'zoomEaseOutMs', icon: 'glide', name: 'Release glide', desc: 'How softly the zoom coasts to a stop.' },
];
export const TOGGLES = [
  { key: 'trackCaret', icon: 'ftc', name: 'Follow the text cursor', desc: 'Follow the caret while you type.' },
  { key: 'trackFocus', icon: 'ffk', name: 'Follow keyboard focus', desc: 'Follow keyboard focus around the screen.' },
  // One segment for both alignment settings (mouseAlign + trackAlign); the flyout writes both.
  { key: 'keepEdges', icon: 'edges', name: 'Keep cursor centred', desc: 'On: the view keeps the pointer, text cursor and focus centred. Off: they move freely and the view follows near the edges.' },
  // #315. A full-width dropdown under the toggle group in the tray that picks the main engine; a change restarts Wind.
  { key: 'engine', icon: 'engine', name: 'Engine', desc: 'A dropdown to switch the engine. Wind restarts to apply it.' },
];
export const KINDS = {
  sliders: { items: SLIDERS, enabledKey: 'traySliders', orderKey: 'traySliderOrder', defaultOn: ['colorWarmPct', 'colorDimPct'], cap: MAX_SLIDERS },
  toggles: { items: TOGGLES, enabledKey: 'trayToggles', orderKey: 'trayToggleOrder', defaultOn: ['trackCaret', 'trackFocus', 'keepEdges', 'engine'], cap: 0 },   // all on by default (#329)
};
export const TRAY_KEYS = ['trayPerf', 'traySliders', 'traySliderOrder', 'trayToggles', 'trayToggleOrder'];

const split = (s) => String(s).split(',').map((t) => t.trim()).filter(Boolean);

// Order = the order key's known items, then the enabled list's known items not yet seen, then every
// remaining eligible item. Enabled = the enabled list (the defaults when its key is absent); enabled
// items beyond the cap read as off, in list order. Returns [{ ...itemMeta, on }].
export function parseList(values, kind) {
  const k = KINDS[kind];
  const eligible = k.items.map((i) => i.key);
  const en = values[k.enabledKey], od = values[k.orderKey];
  const enabled = en === undefined || en === null ? k.defaultOn : split(en);
  const order = [];
  const add = (key) => { if (eligible.includes(key) && !order.includes(key)) order.push(key); };
  if (od !== undefined && od !== null) split(od).forEach(add);
  enabled.forEach(add);
  eligible.forEach(add);
  let on = 0;
  return order.map((key) => {
    const meta = k.items.find((i) => i.key === key);
    const isOn = enabled.includes(key) && (!k.cap || on < k.cap);
    if (isOn) on++;
    return { ...meta, on: isOn };
  });
}

export function parseTray(values) {
  return {
    perf: values.trayPerf === undefined || values.trayPerf === null || String(values.trayPerf).trim() === '1',   // on by default (#329)
    sliders: parseList(values, 'sliders'),
    toggles: parseList(values, 'toggles'),
  };
}

// The two ini entries a list change writes: { [enabledKey]: 'a,b', [orderKey]: 'a,b,c,...' }.
export function serialiseList(kind, items) {
  const k = KINDS[kind];
  return {
    [k.enabledKey]: items.filter((i) => i.on).map((i) => i.key).join(','),
    [k.orderKey]: items.map((i) => i.key).join(','),
  };
}

export const onCount = (items) => items.filter((i) => i.on).length;

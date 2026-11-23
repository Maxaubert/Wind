// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The quick-control lists of a tray or menu bar panel (pure, no DOM): which sliders and toggles it shows
// and in what order, stored in five config keys. Each app passes its own items, defaults and key names:
// Windows (ui/src/tray/trayModel.js) the tray* ini keys, read the same way by src/tray/tray_items.cpp;
// the Mac (mac/ui/src/menubar/menuModel.js) the menu* config.json keys, read the same way by
// mac/Sources/WindCore/MenuLayout.swift.
//
// makeListModel({ sliders, toggles, defaultSliders, defaultToggles, keys, maxSliders })
//   sliders, toggles  [{ key, icon, name, desc, after? }] in default order; `icon` names live in design/icons.js
//   defaultSliders, defaultToggles  the keys that are on when the enabled key is absent
//   keys  { perf, sliders, sliderOrder, toggles, toggleOrder }: the five config key names
//   maxSliders  how many sliders may be on (4)

const split = (s) => String(s).split(',').map((t) => t.trim()).filter(Boolean);

export const onCount = (items) => items.filter((i) => i.on).length;

export function makeListModel({ sliders, toggles, defaultSliders, defaultToggles, keys, maxSliders = 4 }) {
  const KINDS = {
    sliders: { items: sliders, enabledKey: keys.sliders, orderKey: keys.sliderOrder, defaultOn: defaultSliders, cap: maxSliders },
    toggles: { items: toggles, enabledKey: keys.toggles, orderKey: keys.toggleOrder, defaultOn: defaultToggles, cap: 0 },
  };

  // Order = the order key's known items, then the enabled list's known items not yet seen, then every
  // remaining eligible item: after its `after` neighbour when that is in the order, else at the end.
  // Enabled = the enabled list (the defaults when its key is absent); enabled items beyond the cap read
  // as off, in list order. Unknown keys are dropped. Returns [{ ...itemMeta, on }].
  function parseList(values, kind) {
    const k = KINDS[kind];
    const eligible = k.items.map((i) => i.key);
    const en = values[k.enabledKey], od = values[k.orderKey];
    const enabled = en === undefined || en === null ? k.defaultOn : split(en);
    const order = [];
    const add = (key) => { if (eligible.includes(key) && !order.includes(key)) order.push(key); };
    if (od !== undefined && od !== null) split(od).forEach(add);
    enabled.forEach(add);
    k.items.forEach(({ key, after }) => {
      if (order.includes(key)) return;
      const at = after ? order.indexOf(after) : -1;
      if (at >= 0) order.splice(at + 1, 0, key);
      else order.push(key);
    });
    let on = 0;
    return order.map((key) => {
      const meta = k.items.find((i) => i.key === key);
      const isOn = enabled.includes(key) && (!k.cap || on < k.cap);
      if (isOn) on++;
      return { ...meta, on: isOn };
    });
  }

  // Performance header: on by default; values travel as strings, "1" is on.
  function parseTray(values) {
    const p = values[keys.perf];
    return {
      perf: p === undefined || p === null || String(p).trim() === '1',
      sliders: parseList(values, 'sliders'),
      toggles: parseList(values, 'toggles'),
    };
  }

  // The two entries a list change writes: { [enabledKey]: 'a,b', [orderKey]: 'a,b,c,...' }.
  function serialiseList(kind, items) {
    const k = KINDS[kind];
    return {
      [k.enabledKey]: items.filter((i) => i.on).map((i) => i.key).join(','),
      [k.orderKey]: items.map((i) => i.key).join(','),
    };
  }

  // Every key as a config without them reads.
  function defaults() {
    const t = parseTray({});
    return { [keys.perf]: '1', ...serialiseList('sliders', t.sliders), ...serialiseList('toggles', t.toggles) };
  }

  return { MAX_SLIDERS: maxSliders, SLIDERS: sliders, TOGGLES: toggles, KEYS: keys, parseList, parseTray, serialiseList, onCount, defaults };
}

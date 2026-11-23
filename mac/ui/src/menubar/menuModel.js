// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Menu bar panel lists (pure, no DOM): which quick controls the menu bar panel shows and in what order,
// stored in five config.json keys: menuPerf, menuSliders, menuSliderOrder, menuToggles, menuToggleOrder.
// The parse and write rules are Windows' tray rules (ui-shared/src/tray/listModel.js); the Swift side
// reads them the same way (Sources/WindCore/MenuLayout.swift), both checked against
// Tests/fixtures/menu_layout_cases.json. Imported relatively so node tests can load it.
import { makeListModel } from '../../../../ui-shared/src/tray/listModel.js';

export const MAX_SLIDERS = 4;

// `icon` names live in ui-shared/src/design/icons.js. Order here is the default order, and must match
// MenuLayout.sliders / MenuLayout.toggles. Names follow this app's rows.
export const SLIDERS = [
  { key: 'warmthPct', icon: 'warm', name: 'Warmth', desc: 'Screen warmth' },
  { key: 'dimPct', icon: 'bright', name: 'Brightness', desc: 'Screen brightness' },
  { key: 'satPct', icon: 'sat', name: 'Saturation', desc: 'Screen colour strength' },
  { key: 'maxLevel', icon: 'maxz', name: 'Max zoom', desc: 'The highest zoom level' },
  { key: 'zoomInSpeed', icon: 'zin', name: 'Zoom-in speed', desc: 'How fast the view zooms in' },
  { key: 'zoomOutSpeed', icon: 'zout', name: 'Zoom-out speed', desc: 'How fast the view zooms out' },
  { key: 'panSpeed', icon: 'pan', name: 'Arrow key speed', desc: 'How fast the arrow keys move the view' },
  { key: 'zoomEaseOutMs', icon: 'glide', name: 'Release glide', desc: 'How long the zoom coasts after you let go' },
  // The Mac's stand-in for Windows' Pan glide.
  { key: 'panSmoothMs', icon: 'smooth', name: 'Pointer weight', desc: 'Steadies a shaky hand so the view moves calmly' },
];
export const TOGGLES = [
  { key: 'trackCaret', icon: 'ftc', name: 'Follow the text cursor', desc: 'The view follows the text cursor' },
  { key: 'trackFocus', icon: 'ffk', name: 'Follow keyboard focus', desc: 'The view follows keyboard focus' },
  // One segment for both alignment settings (mouseAlign + trackAlign); the panel writes both, as Windows.
  { key: 'keepEdges', icon: 'edges', name: 'Keep cursor centred', desc: 'Keeps the pointer and text cursor centred' },
  { key: 'engine', icon: 'engine', name: 'Engine', desc: 'Switch the engine from the menu bar' },
];

export const MENU_KEYS = { perf: 'menuPerf', sliders: 'menuSliders', sliderOrder: 'menuSliderOrder', toggles: 'menuToggles', toggleOrder: 'menuToggleOrder' };

export const { parseList, parseTray, serialiseList, onCount, defaults, KEYS } = makeListModel({
  sliders: SLIDERS, toggles: TOGGLES, maxSliders: MAX_SLIDERS, keys: MENU_KEYS,
  defaultSliders: ['warmthPct', 'dimPct'],
  defaultToggles: ['trackCaret', 'trackFocus', 'keepEdges', 'engine'],
});

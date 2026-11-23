// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Tray menu lists (pure, no DOM): which quick controls the tray flyout shows and in what order.
// Mirrors src/tray/tray_items.cpp (ParseTrayLayout / WriteTrayLayout) so the page and WindTray read the
// same five global ini keys the same way: trayPerf, traySliders, traySliderOrder, trayToggles,
// trayToggleOrder. Items and limits: docs/specs/2026-10-01-tray-flyout-design.md. The parse and write
// rules are shared with the Mac's menu bar page: ui-shared/src/tray/listModel.js (imported relatively, so
// node tests can load this file without the $shared alias).
import { makeListModel } from '../../../ui-shared/src/tray/listModel.js';

export const MAX_SLIDERS = 4;

// `icon` names live in design/icons.js. Order here is the default order.
export const SLIDERS = [
  { key: 'colorWarmPct', icon: 'warm', name: 'Warmth', desc: 'Screen warmth' },
  { key: 'colorDimPct', icon: 'bright', name: 'Brightness', desc: 'Screen brightness' },
  // `after`: added after the list shipped; a saved order without it gets it next to that item (JoinAfter in tray_items.cpp).
  { key: 'colorSatPct', icon: 'sat', name: 'Saturation', desc: 'Screen colour strength', after: 'colorDimPct' },
  { key: 'maxLevel', icon: 'maxz', name: 'Max zoom', desc: 'The highest zoom level' },
  { key: 'zoomInSpeed', icon: 'zin', name: 'Zoom-in speed', desc: 'How fast the view zooms in' },
  { key: 'zoomOutSpeed', icon: 'zout', name: 'Zoom-out speed', desc: 'How fast the view zooms out' },
  { key: 'panSpeed', icon: 'pan', name: 'Arrow key speed', desc: 'How fast the view pans' },
  { key: 'panGlideMaxPx', icon: 'smooth', name: 'Pan glide', desc: 'How far the pointer coasts after you stop' },
  { key: 'zoomEaseOutMs', icon: 'glide', name: 'Release glide', desc: 'How softly the zoom coasts to a stop' },
];
export const TOGGLES = [
  { key: 'trackCaret', icon: 'ftc', name: 'Follow the text cursor', desc: 'The view follows the text cursor' },
  { key: 'trackFocus', icon: 'ffk', name: 'Follow keyboard focus', desc: 'The view follows keyboard focus' },
  // One segment for both alignment settings (mouseAlign + trackAlign); the flyout writes both.
  { key: 'keepEdges', icon: 'edges', name: 'Keep cursor centred', desc: 'Keeps the pointer and text cursor centred' },
  // #315. A full-width dropdown under the toggle group in the tray that picks the main engine; a change restarts Wind.
  { key: 'engine', icon: 'engine', name: 'Engine', desc: 'Switch the engine from the tray' },
];
export const { parseList, parseTray, serialiseList, onCount, defaults } = makeListModel({
  sliders: SLIDERS, toggles: TOGGLES, maxSliders: MAX_SLIDERS,
  defaultSliders: ['colorWarmPct', 'colorDimPct'],
  defaultToggles: ['trackCaret', 'trackFocus', 'keepEdges', 'engine'],   // all on by default (#329)
  keys: { perf: 'trayPerf', sliders: 'traySliders', sliderOrder: 'traySliderOrder', toggles: 'trayToggles', toggleOrder: 'trayToggleOrder' },
});

// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Settings page layout, Wind for Mac. Five groups: Hotkeys (Settings opens here), Zoom, View, then
// below a divider Preferences and About. Each group has a label, a sidebar icon (src/design/icons.js),
// a one-line description for its banner and cards of rows. Every row key is a real key of the Mac's
// config.json (Sources/WindCore/Config.swift), except the `__` rows, which are actions or Mac status.
// Ranges and defaults follow Config.swift (defaults and clamp()); a slider never offers a value
// clamp() would change.
//
// A card needs two or more rows to carry a caption (a lone row joins a neighbour or sits in an
// uncaptioned card). A row flagged `adv` is an advanced row: it shows inline only while showAdvanced
// is on (Preferences), carries no marker, and search still finds it.
//
// Values travel as strings (bridge.js): bools "1"/"0", numbers as text, bind lists comma-joined.
//
// Row types: keybind, slider, toggle, select, palette (the Theme picker, `wide`: its control sits
// under the text), button (an action: `action` is openIni, exportDiagnostics or a macAction name),
// macstatus (a macOS permission or setting read from getMacStatus, with a fix button: `status` is
// accessibility, screenRecording, zoomShortcuts or loginItem), about. A row may carry showIf
// { key, eq } to hide it unless another setting has that value ({ key, ne } hides it when it does), or
// offIf { key, eq } to keep it in place but dimmed and inert while that setting has the value; `tag`
// adds a small label after the row's name (Experimental).
//
// Keybind rows (controls/Bindings.svelte, PanBinding.svelte): `key` is the Mac config key, its value a
// comma-joined list of Mac bind strings ("mouse4,ctrl+opt+equal", "" = unbound), `max` how many
// bindings it takes. The pan row (`panArrows`) writes its four `panKeys` in one patch. The wheel row
// (`wheelMods`) records modifiers only ("ctrl", "" = off). No on/off switches: an empty bind is off.
//
// Every labelled row carries `keywords`: synonyms and related words people might type (never displayed,
// searched after the label, see src/search/search.js). Add them to any new row.
//
// Copy rules: one short plain line, no punctuation or symbols, no references, no toggle labels starting with "Enable",
// no description that restates its label, no product names.

// The macOS-zoom-only rows (the render engine ignores them).
const ZOOM_ENGINE = { key: 'engine', eq: 'zoom' };

export const groups = [
  { id: 'hotkeys', label: 'Hotkeys', icon: 'hotkeys',
    desc: 'Keys that control zooming',
    cards: [
      { caption: 'Zoom', rows: [
        { key: 'zoomIn', type: 'keybind', max: 3, label: 'Zoom in', keywords: ['magnify', 'enlarge', 'bigger', 'closer', 'hotkey', 'shortcut', 'key', 'keybind', 'binding', 'button', 'mouse button', 'side button', 'increase'],
          desc: 'Hold a key or mouse button to zoom in', def: 'mouse4,ctrl+opt+equal' },
        { key: 'zoomOut', type: 'keybind', max: 3, label: 'Zoom out', keywords: ['shrink', 'smaller', 'reduce', 'back out', 'hotkey', 'shortcut', 'key', 'keybind', 'binding', 'button', 'mouse button', 'side button', 'decrease'],
          desc: 'Hold a key or mouse button to zoom out', def: 'mouse3,ctrl+opt+minus' },
        { key: 'wheelZoomMods', type: 'keybind', wheelMods: true, label: 'Zoom with the scroll wheel', keywords: ['wheel', 'mouse wheel', 'scroll', 'trackpad', 'two fingers', 'swipe', 'modifier', 'control', 'ctrl', 'hotkey', 'shortcut', 'zoom in', 'zoom out'],
          desc: 'Hold these keys and scroll to zoom', def: 'ctrl' },
        { key: 'wheelZoomSpeed', type: 'slider', label: 'Scroll zoom speed', keywords: ['wheel', 'mouse wheel', 'scroll', 'trackpad', 'notch', 'faster', 'slower', 'rate', 'step', 'how fast', 'sensitivity'],
          desc: 'How far each scroll step zooms', min: 0.1, max: 3, step: 0.05, def: 0.6, unit: 'times',
          showIf: { key: 'wheelZoomMods', ne: '' } },
      ] },
      { caption: 'Quick zoom', rows: [
        { key: 'quickZoom', type: 'keybind', max: 2, label: 'Quick zoom', keywords: ['toggle', 'jump', 'instant', 'snap', 'one press', 'preset', 'fixed level', 'hotkey', 'shortcut', 'key', 'keybind'],
          desc: 'Press to jump to a set zoom and back', def: 'ctrl+opt+z' },
        { key: 'quickZoomDefault', type: 'slider', label: 'Quick zoom level', keywords: ['level', 'preset', 'times', 'magnification', 'factor', 'jump', 'toggle', 'instant'],
          desc: 'The zoom a quick zoom jumps to', min: 1.5, max: 16, step: 0.5, def: 4, unit: 'times' },
      ] },
      { caption: 'Extra keys', rows: [
        // The four pan lists are written together; the arrows are drawn by the control.
        { key: '__pan', type: 'keybind', panArrows: true, panKeys: ['panLeft', 'panUp', 'panRight', 'panDown'],
          label: 'Pan with the arrow keys', keywords: ['arrows', 'arrow keys', 'move', 'scroll', 'keyboard', 'navigate', 'shift view', 'left right up down', 'hotkey', 'shortcut', 'modifier'],
          desc: 'Hold modifier keys and an arrow to pan' },
        { key: 'hideCursor', type: 'keybind', max: 2, label: 'Hide pointer', keywords: ['hide mouse', 'hide cursor', 'invisible', 'pointer visibility', 'toggle', 'hotkey', 'shortcut', 'key', 'mouse cursor'],
          desc: 'Hide or show the pointer while zoomed', def: '' },
        { key: 'inspect', type: 'keybind', max: 2, label: 'Inspect mode', keywords: ['freeze', 'frozen', 'crosshair', 'tooltip', 'hover', 'look around', 'reticle', 'lock cursor', 'cursor lock', 'hotkey', 'shortcut', 'key'],
          desc: 'Freeze the pointer so you can look around', def: 'ctrl+opt+i' },
        { key: 'quit', type: 'keybind', max: 2, label: 'Quit Wind', keywords: ['exit', 'close', 'stop', 'emergency', 'escape', 'panic', 'kill', 'hotkey', 'shortcut', 'key'],
          desc: 'Closes Wind from anywhere', def: 'ctrl+opt+escape' },
      ] },
    ] },

  { id: 'zoom', label: 'Zoom', icon: 'zoom',
    desc: 'How far and how fast to zoom',
    cards: [
      { caption: 'Level and speed', rows: [
        { key: 'maxLevel', type: 'slider', label: 'Max zoom', keywords: ['maximum', 'limit', 'highest', 'magnification', 'level', 'cap', 'zoom level', 'factor', 'times', 'upper'], desc: 'The highest zoom level', min: 2, max: 64, step: 1, def: 30, unit: 'times' },
        { key: 'zoomInSpeed', type: 'slider', label: 'Zoom-in speed', keywords: ['faster', 'slower', 'rate', 'magnify speed', 'zoom rate', 'acceleration', 'velocity', 'how fast'], desc: 'How fast the view zooms in', min: 0.25, max: 4, step: 0.05, def: 1.75, unit: 'times' },
        { key: 'zoomOutSpeed', type: 'slider', label: 'Zoom-out speed', keywords: ['faster', 'slower', 'rate', 'zoom rate', 'velocity', 'how fast'], desc: 'How fast the view zooms out', min: 0.25, max: 4, step: 0.05, def: 1.75, unit: 'times' },
        { key: 'zoomEaseOutMs', type: 'slider', label: 'Release glide', keywords: ['inertia', 'coast', 'momentum', 'ease out', 'easing', 'smooth stop', 'slow down', 'deceleration', 'stop', 'let go', 'release', 'delay', 'milliseconds'], desc: 'How long the zoom coasts after you let go', min: 0, max: 400, step: 5, def: 115, unit: 'ms' },
      ] },
      { caption: 'Easing', rows: [
        { key: 'smoothZoom', type: 'toggle', adv: true, label: 'Soft start', keywords: ['ease', 'ease in', 'acceleration', 'gentle', 'smooth', 'smoothing', 'ramp', 'curve', 'animation', 'start'], desc: 'Each zoom starts slowly and speeds up', def: 1 },
        { key: 'smoothZoomAccel', type: 'slider', adv: true, label: 'Soft start strength', keywords: ['ease', 'ease in', 'acceleration', 'gentle', 'smooth', 'smoothing', 'ramp', 'curve', 'animation', 'start'], desc: 'How gently each zoom begins', min: 1, max: 10, step: 0.5, def: 3,
          offIf: { key: 'smoothZoom', eq: '0' } },
        { key: 'smoothZoomRamp', type: 'slider', adv: true, label: 'Soft start length', keywords: ['ease', 'duration', 'time', 'ramp', 'seconds', 'how long', 'smooth', 'smoothing', 'animation', 'curve'], desc: 'How long the soft start lasts', min: 0, max: 3, step: 0.1, def: 0.6, unit: 'seconds',
          offIf: { key: 'smoothZoom', eq: '0' } },
      ] },
      { caption: 'Engine', rows: [
        { key: 'smoothImages', type: 'select', label: 'Scaling', keywords: ['scaling', 'upscaling', 'filter', 'sharp', 'crisp', 'smooth', 'blurry', 'pixelated', 'sampling', 'quality', 'nearest', 'linear', 'bilinear', 'antialiasing', 'images'],
          desc: 'How the image is enlarged', options: ['0', '1'], optionLabels: { '0': 'Nearest', '1': 'Smooth' }, def: '0' },
        // Switches live; Wind falls back to the macOS zoom when it cannot capture the screen.
        { key: 'engine', type: 'select', adv: true, label: 'Engine', keywords: ['renderer', 'render', 'mode', 'model', 'backend', 'method', 'magnifier', 'capture', 'draw', 'system zoom', 'macos zoom', 'accessibility zoom'],
          desc: 'How the screen is zoomed', options: ['draw', 'zoom'], optionLabels: { draw: 'Render', zoom: 'macOS zoom' }, def: 'draw' },
      ] },
    ] },

  { id: 'view', label: 'View', icon: 'view',
    desc: 'How the view follows the mouse and typing',
    cards: [
      { caption: 'Movement', rows: [
        { key: 'panSpeed', type: 'slider', label: 'Arrow key speed', keywords: ['pan', 'pan speed', 'arrows', 'move', 'scroll', 'keyboard', 'how fast', 'velocity', 'shift view', 'rate'], desc: 'How fast the arrow keys move the view', min: 0.25, max: 4, step: 0.05, def: 1, unit: 'times' },
        { key: 'cursorSensitivity', type: 'slider', label: 'Inspect mouse speed', keywords: ['sensitivity', 'pointer speed', 'cursor speed', 'mouse speed', 'inspect', 'look around', 'locked', 'game', 'how fast', 'velocity', 'rate'], desc: 'How fast the view moves in Inspect mode', min: 0.25, max: 4, step: 0.05, def: 1, unit: 'times' },
        // panSmoothMs: on the render engine a low-pass on the pointer (it gets weight against hand
        // jitter while the view stays centred on it); on the macOS zoom a glide over whole-point steps.
        { key: 'panSmoothMs', type: 'slider', label: 'Pointer weight', keywords: ['jitter', 'shake', 'tremor', 'steady', 'smooth', 'smoothing', 'glide', 'inertia', 'heavy', 'weight', 'filter', 'mouse', 'pan', 'stabilise', 'stabilize', 'milliseconds'],
          desc: 'Steadies a shaky hand so the view moves calmly', min: 0, max: 500, step: 10, def: 0, unit: 'ms' },
        { key: 'mouseAlign', type: 'select', label: 'Pointer position', keywords: ['mouse position', 'mouse', 'cursor', 'pointer', 'where', 'centred', 'centered', 'center', 'centre', 'middle', 'edge', 'edges', 'within the edges', 'align', 'alignment', 'lock to center', 'follow mode', 'placement'], desc: 'Where the pointer sits while the view moves', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
        { key: 'mouseMarginPct', type: 'slider', label: 'Edge margin', keywords: ['edge', 'edges', 'border', 'padding', 'distance', 'boundary', 'margin', 'pointer', 'mouse', 'percent', 'dead zone', 'how near'], desc: 'How close to the edge before the view moves', min: 0, max: 40, step: 1, def: 0, unit: '%',
          showIf: { key: 'mouseAlign', eq: '1' } },
      ] },
      { caption: 'Typing and focus', rows: [
        { key: 'trackCaret', type: 'toggle', label: 'Follow the text cursor', keywords: ['caret', 'typing', 'type', 'text', 'keyboard', 'insertion point', 'track', 'tracking', 'editor', 'word', 'input', 'write', 'follow'], desc: 'The view moves to where you type', def: 1 },
        { key: 'trackFocus', type: 'toggle', label: 'Follow keyboard focus', keywords: ['focus', 'tab', 'tab key', 'navigate', 'selected', 'control', 'button', 'accessibility', 'keyboard', 'track', 'tracking', 'highlight', 'follow'], desc: 'The view moves to the selected control', def: 0 },
        { key: 'trackAlign', type: 'select', label: 'Text cursor position', keywords: ['caret', 'typing', 'where', 'centred', 'centered', 'center', 'centre', 'middle', 'edge', 'edges', 'within the edges', 'align', 'alignment', 'focus', 'placement', 'cursor'], desc: 'Where the text cursor and focus sit', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
        { key: 'trackMarginPct', type: 'slider', label: 'Text edge margin', keywords: ['edge', 'edges', 'border', 'padding', 'distance', 'margin', 'caret', 'typing', 'focus', 'percent', 'how near'], desc: 'How close the text cursor gets to the edge', min: 0, max: 40, step: 1, def: 15, unit: '%',
          showIf: { key: 'trackAlign', eq: '1' } },
        { key: 'trackGlideMs', type: 'slider', label: 'Typing glide', keywords: ['glide', 'smooth', 'animation', 'ease', 'jump', 'caret', 'typing', 'focus', 'how fast', 'milliseconds', 'duration'], desc: 'How long the view takes to reach the text cursor', min: 0, max: 600, step: 10, def: 200, unit: 'ms' },
        { key: 'hidePointerWhileTracking', type: 'toggle', label: 'Hide the pointer while typing', keywords: ['hide mouse', 'hide cursor', 'pointer', 'typing', 'caret', 'invisible', 'distraction', 'focus'], desc: 'The pointer comes back when you move the mouse', def: 1 },
      ] },
      { caption: 'macOS zoom engine', rows: [
        { key: 'keepPointerSpeed', type: 'toggle', label: 'Keep pointer speed', keywords: ['pointer speed', 'mouse speed', 'cursor speed', 'slow', 'sluggish', 'acceleration', 'tracking speed', 'zoomed', 'same distance'],
          desc: 'The pointer covers the same distance at any zoom', def: 1, showIf: ZOOM_ENGINE },
        { key: 'slowPrecision', type: 'toggle', label: 'Fine steps for slow moves', keywords: ['precision', 'slow', 'fine', 'steps', 'exact', 'accuracy', 'small moves', 'pixel'],
          desc: 'Very slow moves keep the small zoomed steps', def: 0, showIf: ZOOM_ENGINE },
        { key: 'precisionBelow', type: 'slider', adv: true, label: 'Fine steps below', keywords: ['precision', 'threshold', 'slow', 'fine', 'speed', 'points per second', 'hand speed'],
          desc: 'The hand speed under which fine steps apply', min: 20, max: 400, step: 10, def: 100,
          showIf: ZOOM_ENGINE, offIf: { key: 'slowPrecision', eq: '0' } },
        { key: 'slowPanGlide', type: 'toggle', label: 'Smooth slow pans', tag: 'Experimental', keywords: ['glide', 'smooth', 'slow', 'pan', 'steps', 'stutter', 'judder', 'whole point'],
          desc: 'Glides each small step of a slow pan', def: 0, showIf: ZOOM_ENGINE },
        { key: 'cursor', type: 'select', label: 'Pointer while zoomed', keywords: ['cursor', 'pointer', 'mouse', 'draw', 'steady', 'shake', 'wobble', 'system cursor', 'own cursor'],
          desc: 'Who draws the pointer while zoomed', options: ['system', 'wind'], optionLabels: { system: 'macOS', wind: 'Wind' }, def: 'system', showIf: ZOOM_ENGINE },
      ] },
    ] },

  { id: 'prefs', label: 'Preferences', icon: 'general', bottom: true,
    desc: 'Appearance and everyday options',
    cards: [
      { caption: 'General', rows: [
        { key: 'uiPalette', type: 'palette', label: 'Theme', keywords: ['palette', 'colour', 'color', 'colours', 'colors', 'appearance', 'look', 'dark', 'grey', 'gray', 'ember', 'ocean', 'high contrast', 'hicon', 'skin', 'style', 'accent', 'ui', 'interface'], desc: 'Pick the look of the app', def: 'grey' },
        { key: 'showAdvanced', type: 'toggle', label: 'Show advanced settings', keywords: ['expert', 'extra', 'more', 'hidden', 'options', 'all settings', 'power user', 'developer', 'reveal', 'show all', 'additional', 'engine'], desc: 'Shows extra options on every page', def: 0 },
      ] },
      { caption: 'Screen light', rows: [
        { key: 'warmthPct', type: 'slider', label: 'Warmth', keywords: ['night light', 'night shift', 'blue light', 'yellow', 'orange', 'warm', 'colour temperature', 'color temperature', 'temperature', 'eye strain', 'tint', 'filter', 'evening', 'sleep', 'amber', 'red', 'colour', 'color', 'reduce blue', 'kelvin'], desc: 'Makes the screen warmer', min: 0, max: 100, step: 5, def: 0, unit: '%' },
        { key: 'dimPct', type: 'slider', label: 'Brightness', keywords: ['dim', 'dark', 'darker', 'dimmer', 'light', 'luminance', 'backlight', 'night', 'screen', 'intensity', 'contrast', 'colour', 'color', 'lower', 'reduce'], desc: 'Dims the whole screen', min: 10, max: 100, step: 5, def: 100, unit: '%' },
      ] },
      // Mac status (getMacStatus / macStatus pushes) and the fixes (macAction).
      { caption: 'macOS', rows: [
        { key: '__accessibility', type: 'macstatus', status: 'accessibility', label: 'Accessibility', keywords: ['permission', 'privacy', 'security', 'access', 'allow', 'trust', 'input', 'keys', 'mouse', 'system settings', 'not working'],
          desc: 'Lets Wind see your keys and move the view', on: 'Allowed', off: 'Not allowed', btn: 'Open System Settings', action: 'openAccessibility' },
        { key: '__screenRecording', type: 'macstatus', status: 'screenRecording', label: 'Screen Recording', keywords: ['permission', 'privacy', 'security', 'capture', 'screen capture', 'record', 'allow', 'render', 'draw', 'engine', 'system settings', 'black screen'],
          desc: 'Lets Wind draw the zoomed screen itself', on: 'Allowed', off: 'Not allowed', btn: 'Open System Settings', action: 'openScreenRecording' },
        { key: '__zoomShortcuts', type: 'macstatus', status: 'zoomShortcuts', label: 'macOS Zoom shortcuts', keywords: ['clash', 'conflict', 'system zoom', 'accessibility zoom', 'scroll gesture', 'keyboard shortcuts', 'trackpad', 'turn off', 'disable', 'double zoom'],
          desc: 'They fight Wind for the zoom when they are on', on: 'Off', off: 'On', btn: 'Turn off', action: 'fixZoomShortcuts' },
        { key: '__loginItem', type: 'macstatus', status: 'loginItem', toggle: true, label: 'Open at login', keywords: ['startup', 'start up', 'launch', 'boot', 'login items', 'autostart', 'automatically', 'sign in', 'background'],
          desc: 'Starts Wind when you log in', action: 'setLogin' },
        { key: 'logLevel', type: 'select', label: 'Log detail', keywords: ['log', 'logging', 'verbose', 'debug', 'diagnostics', 'trace', 'quiet', 'file', 'troubleshoot'],
          desc: 'How much Wind writes to its log', options: ['0', '1', '2', '3'], optionLabels: { '0': 'Quiet', '1': 'Sessions', '2': 'Verbose', '3': 'Every zoom step' }, def: '1' },
        { key: '__showLog', type: 'button', label: 'Show log', keywords: ['log', 'logs', 'console', 'debug', 'troubleshoot', 'problem', 'file', 'history'],
          desc: 'Opens the log file', btn: 'Show', action: 'showLog' },
        { key: '__openIni', type: 'button', label: 'Edit settings file', keywords: ['config', 'config.json', 'json', 'edit', 'text editor', 'manual', 'raw', 'configuration', 'file'],
          desc: 'Opens the settings file in a text editor', btn: 'Open', action: 'openIni' },
        { key: '__diagnostics', type: 'button', label: 'Export diagnostics', keywords: ['report', 'support', 'help', 'log', 'logs', 'zip', 'bug', 'troubleshoot', 'problem', 'debug', 'crash', 'save report', 'share'],
          desc: 'Saves a report to share when asking for help', btn: 'Export', action: 'exportDiagnostics' },
        { key: '__rerunSetup', type: 'button', label: 'Run setup again', keywords: ['onboarding', 'welcome', 'first run', 'wizard', 'setup', 'permissions', 'start over', 'tour'],
          desc: 'Walks through the first start steps', btn: 'Start', action: 'rerunSetup' },
      ] },
    ] },

  { id: 'about', label: 'About', icon: 'about', desc: '', bottom: true,
    cards: [
      { caption: '', bare: true, rows: [{ key: '__about', type: 'about' }] },
    ] },
];

// Flat row list of one group, in page order.
export const groupRows = (g) => g.cards.flatMap((c) => c.rows);
export const allRows = groups.flatMap(groupRows);

// The config keys a row reads or writes: its own, or the four pan keys of the pan row.
export const rowKeys = (r) => (r.panKeys ? r.panKeys : r.key.startsWith('__') ? [] : [r.key]);

// Defaults of every config key the page shows (as the strings the host sends). The pan keys default
// to ctrl+opt plus their arrow (Config.swift).
export function schemaDefaults() {
  const d = { panLeft: 'ctrl+opt+left', panUp: 'ctrl+opt+up', panRight: 'ctrl+opt+right', panDown: 'ctrl+opt+down' };
  for (const r of allRows) if (!r.key.startsWith('__') && r.def !== undefined) d[r.key] = String(r.def);
  return d;
}

// A macOS permission is missing that Wind needs now: Accessibility always, Screen Recording on the
// render engine. Drives the page banner (shell/PermissionBanner.svelte).
export function missingPermissions(mac, engine) {
  if (!mac) return [];
  const out = [];
  if (mac.accessibility === false) out.push('accessibility');
  if (mac.screenRecording === false && engine === 'draw') out.push('screenRecording');
  return out;
}

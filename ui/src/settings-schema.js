// Settings page layout (#318, round 2 of the redesign #303). Seven groups: Hotkeys (Settings opens
// here), Zoom, View, Screen, then below a divider Preferences, Tray menu, About. Each group has a
// label, a sidebar icon (src/design/icons.js), a one-line description for its banner and cards of
// rows. Names and descriptions follow the ia07 mockup copy table (decisions log, "Copy rules").
//
// A card needs two or more rows to carry a caption (a lone row joins a neighbour or sits in an
// uncaptioned card). A row flagged `adv` is an advanced row: it shows inline only while the global
// showAdvanced key is on (Preferences), carries no marker, and search still finds it.
//
// Row types: keybind, slider, toggle, select, applist, highres, engine (the engine select with the
// inline "Restart Wind"), palette (the Theme picker, `wide`: its control sits under the
// text), profiles (dropdown + New), button, about. A row may carry showIf
// { key, eq } to hide it unless another setting has that value ({ key, ne } hides it when it does), or
// offIf { key, eq } to keep it in place but dimmed and inert while that setting has the value; `tag`
// adds a small label after the row's name (Experimental). A keybind row may carry `max` (how
// many bindings it takes, default 1) and `onKey` (the on/off switch key of an extra key).
//
// Every labelled row carries `keywords`: synonyms and related words people might type (never displayed,
// searched after the label, see src/search/search.js). Add them to any new row; tests/search.spec.js checks it.
//
// Copy rules: one short plain line, no punctuation or symbols, no references, no toggle labels starting with "Enable",
// no description that restates its label, no product names.

const hybrid = { key: 'model', eq: 'hybrid' };
const engineOpts = ['auto', 'transform', 'render'];
const engineLabels = { auto: 'Auto', transform: 'Transform', render: 'Render' };

export const groups = [
  { id: 'hotkeys', label: 'Hotkeys', icon: 'hotkeys',
    desc: 'Keys that control zooming',
    cards: [
      { caption: 'Zoom', rows: [
        // Two bindings per direction (max 2). Each ini slot holds one part: a key, a mouse button or a
        // wheel direction (button codes 6 / 7), with its modifiers. Either slot works alone, both fire
        // the same action (the core OR-combines them).
        { key: '__zoomIn', type: 'keybind', max: 2, label: 'Zoom in', keywords: ['magnify', 'enlarge', 'bigger', 'closer', 'hotkey', 'shortcut', 'key', 'keybind', 'binding', 'button', 'mouse wheel', 'wheel', 'scroll', 'increase'],
          desc: 'Hold a key or scroll to zoom in',
          buttonKey: 'zoomInButton', vkKey: 'zoomInVk', modsKey: 'zoomInMods', buttonModsKey: 'zoomInButtonMods',
          buttonKey2: 'zoomInButton2', vkKey2: 'zoomInVk2', modsKey2: 'zoomInMods2', buttonModsKey2: 'zoomInButton2Mods' },
        { key: '__zoomOut', type: 'keybind', max: 2, label: 'Zoom out', keywords: ['shrink', 'smaller', 'reduce', 'back out', 'hotkey', 'shortcut', 'key', 'keybind', 'binding', 'button', 'mouse wheel', 'wheel', 'scroll', 'decrease'], desc: 'Hold a key or scroll to zoom out',
          buttonKey: 'zoomOutButton', vkKey: 'zoomOutVk', modsKey: 'zoomOutMods', buttonModsKey: 'zoomOutButtonMods',
          buttonKey2: 'zoomOutButton2', vkKey2: 'zoomOutVk2', modsKey2: 'zoomOutMods2', buttonModsKey2: 'zoomOutButton2Mods' },
        // Keyboard-hook suspension (#156): trades key-interception for smooth panning, per app.
        { key: 'noSwallowApps', type: 'applist', adv: true, label: 'Share zoom keys with these apps', keywords: ['exclude', 'exception', 'passthrough', 'pass through', 'allow', 'whitelist', 'block', 'swallow', 'conflict', 'game', 'program', 'exe', 'hook', 'keys'],
          desc: 'Zoom keys also reach these apps', def: '' },
      ] },
      { caption: 'Extra keys', rows: [
        // Keyboard panning (#287): unbound by default (#307). The arrow keys are fixed and drawn by the
        // control itself; only the modifiers (one or two, required) are chosen. The switch (panKeysOn)
        // frees the keys without losing the binding.
        { key: '__pan', type: 'keybind', label: 'Pan with the arrow keys', keywords: ['arrows', 'arrow keys', 'move', 'scroll', 'keyboard', 'navigate', 'shift view', 'left right up down', 'hotkey', 'shortcut', 'modifier'],
          desc: 'Hold modifier keys and an arrow to pan',
          panArrows: true, onKey: 'panKeysOn',
          panKeys: ['panLeftVk', 'panUpVk', 'panRightVk', 'panDownVk', 'panLeftMods', 'panUpMods', 'panRightMods', 'panDownMods'] },
        { key: '__hideCursor', type: 'keybind', label: 'Hide pointer', keywords: ['hide mouse', 'hide cursor', 'invisible', 'pointer visibility', 'toggle', 'hotkey', 'shortcut', 'key', 'mouse cursor'], desc: 'Hide or show the pointer while zoomed',
          vkKey: 'hideCursorVk', modsKey: 'hideCursorMods', onKey: 'hideCursorOn' },
        { key: '__cursorLock', type: 'keybind', label: 'Inspect mode', keywords: ['freeze', 'frozen', 'crosshair', 'tooltip', 'hover', 'look around', 'reticle', 'lock cursor', 'cursor lock', 'hotkey', 'shortcut', 'key'], desc: 'Freeze the pointer so you can look around',
          vkKey: 'cursorLockVk', modsKey: 'cursorLockMods', onKey: 'cursorLockOn' },
      ] },
    ] },

  { id: 'zoom', label: 'Zoom', icon: 'zoom',
    desc: 'How far and how fast to zoom',
    cards: [
      { caption: 'Level and speed', rows: [
        { key: 'maxLevel', type: 'slider', label: 'Max zoom', keywords: ['maximum', 'limit', 'highest', 'magnification', 'level', 'cap', 'zoom level', 'factor', 'times', 'upper'], desc: 'The highest zoom level', min: 2, max: 50, step: 1, def: 12.0, unit: 'times' },
        { key: 'zoomInSpeed', type: 'slider', label: 'Zoom-in speed', keywords: ['faster', 'slower', 'rate', 'magnify speed', 'zoom rate', 'acceleration', 'velocity', 'how fast'], desc: 'How fast the view zooms in', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'zoomOutSpeed', type: 'slider', label: 'Zoom-out speed', keywords: ['faster', 'slower', 'rate', 'zoom rate', 'velocity', 'how fast'], desc: 'How fast the view zooms out', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'zoomEaseOutMs', type: 'slider', label: 'Release glide', keywords: ['inertia', 'coast', 'momentum', 'ease out', 'easing', 'smooth stop', 'slow down', 'deceleration', 'stop', 'let go', 'release', 'delay', 'milliseconds'], desc: 'How long the zoom coasts after you let go', min: 0, max: 300, step: 5, def: 45, unit: 'ms',
          offIf: { key: 'txSamplingMode', eq: '1' } },
      ] },
      { caption: 'Easing', rows: [
        // Smooth zoom is always on (core default 1); its shape sliders are advanced.
        { key: 'smoothZoomAccel', type: 'slider', adv: true, label: 'Soft start', keywords: ['ease', 'ease in', 'acceleration', 'gentle', 'smooth', 'smoothing', 'ramp', 'curve', 'animation', 'start'], desc: 'Softens the start of each zoom', min: 1, max: 8, step: 0.5, def: 3.0 },
        { key: 'smoothZoomRamp', type: 'slider', adv: true, label: 'Soft start length', keywords: ['ease', 'duration', 'time', 'ramp', 'seconds', 'how long', 'smooth', 'smoothing', 'animation', 'curve'], desc: 'How long the soft start lasts', min: 0.1, max: 3, step: 0.1, def: 0.6, unit: 'seconds' },
      ] },
      { caption: 'Engine', rows: [
        { key: 'model', type: 'engine', adv: true, label: 'Engine', keywords: ['renderer', 'render', 'mode', 'transform', 'auto', 'hybrid', 'model', 'backend', 'method', 'magnifier', 'gpu', 'capture', 'dwm', 'restart', 'compositor'], desc: 'How the screen is zoomed',
          options: ['hybrid', 'render', 'transform'],
          optionLabels: { hybrid: 'Auto', render: 'Render', transform: 'Transform' },
          def: 'hybrid' },
        // PER-WINDOW-TYPE ENGINE (2026-08-24). Every row defaults to Auto so an untouched install
        // behaves as before. They only apply when the engine above is Auto, so showIf hides them
        // when a single engine is pinned. ("Never use Render for" left Settings in #318; the
        // renderExclude ini key and the core's own detection stay.)
        { key: 'engineGame', type: 'select', adv: true, label: 'Engine for games', keywords: ['renderer', 'render', 'transform', 'auto', 'fullscreen', 'borderless', 'video games', 'gaming', 'mode', 'per window', 'per app'], desc: 'Full screen and borderless games',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineAcrylic', type: 'select', adv: true, label: 'Engine for blurred windows', keywords: ['renderer', 'render', 'transform', 'auto', 'acrylic', 'mica', 'transparent', 'translucent', 'glass', 'blur', 'mode', 'per window'],
          desc: 'Windows with blurred see through backgrounds',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineDesktop', type: 'select', adv: true, label: 'Engine for the desktop', keywords: ['renderer', 'render', 'transform', 'auto', 'taskbar', 'explorer', 'file windows', 'shell', 'mode', 'per window'],
          desc: 'The desktop taskbar and file windows',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineOther', type: 'select', adv: true, label: 'Engine for other windows', keywords: ['renderer', 'render', 'transform', 'auto', 'apps', 'programs', 'rest', 'everything else', 'default', 'mode', 'per window'], desc: 'All other windows',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
      ] },
    ] },

  { id: 'view', label: 'View', icon: 'view',
    desc: 'How the view follows the mouse and typing',
    cards: [
      { caption: 'Speed', rows: [
        { key: 'panSpeed', type: 'slider', label: 'Arrow key speed', keywords: ['pan', 'pan speed', 'arrows', 'move', 'scroll', 'keyboard', 'how fast', 'velocity', 'shift view', 'rate'], desc: 'How fast the arrow keys move the view', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'cursorSensitivity', type: 'slider', label: 'Mouse speed', keywords: ['sensitivity', 'pointer speed', 'cursor speed', 'dpi', 'pan', 'follow', 'tracking speed', 'how fast', 'velocity', 'mouse sensitivity', 'rate'], desc: 'How fast the view moves with the mouse', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'panGlideMaxPx', type: 'slider', label: 'Pan glide', keywords: ['inertia', 'momentum', 'glide', 'coast', 'ease', 'ease out', 'soft stop', 'stop', 'slide', 'smooth', 'smoothing', 'mouse', 'pan', 'drift', 'deceleration'], desc: 'How far the pointer coasts after you stop', min: 0, max: 200, step: 5, def: 0, unit: 'px' },
      ] },
      { caption: 'Pointer', rows: [
        { key: 'mouseAlign', type: 'select', label: 'Pointer position', keywords: ['mouse position', 'mouse', 'cursor', 'pointer', 'where', 'centred', 'centered', 'center', 'centre', 'middle', 'edge', 'edges', 'within the edges', 'align', 'alignment', 'lock to center', 'follow mode', 'placement'], desc: 'Where the pointer sits while the view moves', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
        { key: 'mouseMarginPct', type: 'slider', label: 'Edge margin', keywords: ['edge', 'edges', 'border', 'padding', 'distance', 'boundary', 'margin', 'pointer', 'mouse', 'percent', 'dead zone', 'how near'], desc: 'How close to the edge before the view moves', min: 0, max: 40, step: 1, def: 0, unit: '%',
          showIf: { key: 'mouseAlign', eq: '1' } },
        // High resolution cursor (#227). Ini key txSamplingMode (0 nearest / 1 smooth). No longer
        // coupled to MPO (#369): both looks keep apps off hardware planes while zoomed (smooth via the
        // resample layer, nearest via the MPO guard), so the toggle applies live with no restart.
        { key: 'txSamplingMode', type: 'highres', label: 'High resolution cursor', tag: 'Experimental', keywords: ['sharp', 'crisp', 'smooth', 'blurry', 'pixelated', 'hidpi', 'sampling', 'quality', 'nearest', 'mpo', 'overlay', 'cursor', 'pointer', 'resolution', '4k', 'antialiasing', 'restart', 'registry'],
          desc: 'A smoother image and pointer at high zoom', def: 0 },
        // Zoom lock detection (#221): games like DOOM pin the mouse to the screen centre, which would
        // pin the zoom view there too. Listed apps get the view UNLOCKED from the pointer.
        { key: 'lockApps', type: 'applist', adv: true, label: 'Mouse-locked games', keywords: ['lock', 'locked', 'fps', 'shooter', 'first person', 'mouselook', 'mouse look', 'camera', 'game', 'exe', 'program', 'unlock', 'exception', 'apps', 'pinned', 'centre'],
          desc: 'The view follows the mouse in these games', def: '' },
      ] },
      { caption: 'Typing and focus', rows: [
        { key: 'trackCaret', type: 'toggle', label: 'Follow the text cursor', keywords: ['caret', 'typing', 'type', 'text', 'keyboard', 'insertion point', 'track', 'tracking', 'editor', 'notepad', 'word', 'input', 'write', 'follow'], desc: 'The view moves to where you type', def: 1 },
        { key: 'trackFocus', type: 'toggle', label: 'Follow keyboard focus', keywords: ['focus', 'tab', 'tab key', 'navigate', 'selected', 'control', 'button', 'accessibility', 'keyboard', 'track', 'tracking', 'highlight', 'follow'], desc: 'The view moves to the selected control', def: 0 },
        { key: 'trackAlign', type: 'select', label: 'Text cursor position', keywords: ['mouse position', 'caret', 'typing', 'where', 'centred', 'centered', 'center', 'centre', 'middle', 'edge', 'edges', 'within the edges', 'align', 'alignment', 'focus', 'placement', 'mouse', 'cursor'], desc: 'Where the text cursor and focus sit', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
      ] },
    ] },

  { id: 'prefs', label: 'Preferences', icon: 'general', bottom: true,
    desc: 'Appearance and everyday options',
    cards: [
      { caption: 'General', rows: [
        // Global UI-only key (uiPalette): the built-in theme, shared by Settings and the tray menu. One row of mini window cards, right-aligned like the other controls.
        { key: 'uiPalette', type: 'palette', label: 'Theme', keywords: ['palette', 'colour', 'color', 'colours', 'colors', 'appearance', 'look', 'dark', 'grey', 'gray', 'ember', 'ocean', 'high contrast', 'hicon', 'skin', 'style', 'accent', 'ui', 'interface'], desc: 'Pick the look of the app', def: 'grey' },
        { key: '__profiles', type: 'profiles', label: 'Profile', keywords: ['preset', 'config', 'configuration', 'save', 'saved', 'profiles', 'switch', 'load', 'new', 'create', 'delete', 'rename', 'duplicate', 'set of settings', 'game profile', 'scheme', 'layout'], desc: 'A saved set of all settings' },
        // Global UI-only key: shows the advanced rows of every page.
        { key: 'showAdvanced', type: 'toggle', label: 'Show advanced settings', keywords: ['expert', 'extra', 'more', 'hidden', 'options', 'all settings', 'power user', 'developer', 'reveal', 'show all', 'additional'], desc: 'Shows extra options on every page', def: 0 },
      ] },
      // Screen light (#288): warmth and brightness, one DWM colour matrix (the render engine applies it
      // in its shader). Was its own page; two sliders read better next to the other preferences (#423).
      { caption: 'Screen light', rows: [
        { key: 'colorWarmPct', type: 'slider', label: 'Warmth', keywords: ['night light', 'blue light', 'yellow', 'orange', 'warm', 'colour temperature', 'color temperature', 'temperature', 'eye strain', 'tint', 'filter', 'evening', 'sleep', 'amber', 'red', 'colour', 'color', 'reduce blue', 'kelvin'], desc: 'Makes the screen warmer', min: 0, max: 100, step: 5, def: 0, unit: '%' },
        { key: 'colorDimPct', type: 'slider', label: 'Brightness', keywords: ['dim', 'dark', 'darker', 'dimmer', 'light', 'luminance', 'backlight', 'night', 'screen', 'intensity', 'contrast', 'colour', 'color', 'lower', 'reduce'], desc: 'Dims the whole screen', min: 1, max: 100, step: 1, def: 100, unit: '%' },
      ] },
      { caption: 'Troubleshooting', rows: [
        // Always visible: these are not advanced settings.
        { key: 'diagnostics', type: 'toggle', label: 'Frame time logging', keywords: ['log', 'logging', 'fps', 'performance', 'diagnostics', 'debug', 'stutter', 'profiling', 'telemetry', 'trace', 'measure', 'benchmark', 'file'], desc: 'Writes frame times to a log file', def: 0 },
        { key: '__diagnostics', type: 'button', label: 'Export diagnostics', keywords: ['report', 'support', 'help', 'log', 'logs', 'zip', 'bug', 'troubleshoot', 'problem', 'debug', 'crash', 'save report', 'share'], desc: 'Saves a report to share when asking for help', btn: 'Export', action: 'exportDiagnostics' },
        { key: '__openIni', type: 'button', label: 'Open settings file', keywords: ['ini', 'magnifier.ini', 'edit', 'config', 'text editor', 'notepad', 'manual', 'raw', 'configuration', 'file'], desc: 'Opens the settings file in a text editor', btn: 'Open', action: 'openIni' },
      ] },
    ] },

  // Tray menu (#313): the Performance switch is an ordinary row; the two item lists are drawn by
  // src/tray/TrayMenuPage.svelte (custom: 'tray'). All five tray keys are global, never profile keys.
  { id: 'tray', label: 'Tray menu', icon: 'tray', custom: 'tray', bottom: true,
    desc: 'What the tray menu shows and in what order',
    cards: [
      { caption: '', rows: [
        { key: 'trayPerf', type: 'toggle', label: 'Performance in the tray', keywords: ['tray', 'menu', 'fps', 'frame time', 'graph', 'stats', 'statistics', 'zoom level', 'monitor', 'flyout', 'notification area', 'system tray', 'show', 'hide'], desc: 'Shows live zoom and frame stats', def: 1 },
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

// The ini keys a keybind row reads or writes besides its own: its real state lives under
// buttonKey/vkKey/modsKey (and the *2 slot, and the four pan keys), so loading and diffing cover those.
export const bindKeys = (r) =>
  ['buttonKey', 'vkKey', 'modsKey', 'buttonModsKey', 'buttonKey2', 'vkKey2', 'modsKey2', 'buttonModsKey2']
    .map((k) => r[k]).filter(Boolean).concat(r.panKeys || []);

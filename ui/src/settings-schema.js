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
// { key, eq } to hide it unless another setting has that value. A keybind row may carry `max` (how
// many bindings it takes, default 1) and `onKey` (the on/off switch key of an extra key).
//
// Copy rules: plain language, no toggle labels starting with "Enable", no description that restates
// its label, no symbols or product names.

const hybrid = { key: 'model', eq: 'hybrid' };
const engineOpts = ['auto', 'transform', 'render'];
const engineLabels = { auto: 'Auto', transform: 'Transform', render: 'Render' };

export const groups = [
  { id: 'hotkeys', label: 'Hotkeys', icon: 'hotkeys',
    desc: 'Keys that control the magnifier.',
    cards: [
      { caption: 'Zoom', rows: [
        // Two bindings per direction (max 2). Each ini slot holds one part: a key, a mouse button or a
        // wheel direction (button codes 6 / 7), with its modifiers. Either slot works alone, both fire
        // the same action (the core OR-combines them).
        { key: '__zoomIn', type: 'keybind', max: 2, label: 'Zoom in',
          desc: 'Hold or scroll to magnify the view.',
          buttonKey: 'zoomInButton', vkKey: 'zoomInVk', modsKey: 'zoomInMods', buttonModsKey: 'zoomInButtonMods',
          buttonKey2: 'zoomInButton2', vkKey2: 'zoomInVk2', modsKey2: 'zoomInMods2', buttonModsKey2: 'zoomInButton2Mods' },
        { key: '__zoomOut', type: 'keybind', max: 2, label: 'Zoom out', desc: 'Hold or scroll to zoom back out.',
          buttonKey: 'zoomOutButton', vkKey: 'zoomOutVk', modsKey: 'zoomOutMods', buttonModsKey: 'zoomOutButtonMods',
          buttonKey2: 'zoomOutButton2', vkKey2: 'zoomOutVk2', modsKey2: 'zoomOutMods2', buttonModsKey2: 'zoomOutButton2Mods' },
        // Keyboard-hook suspension (#156): trades key-interception for smooth panning, per app.
        { key: 'noSwallowApps', type: 'applist', adv: true, label: 'Share zoom keys with these apps',
          desc: 'Zoom keys also reach these apps.', def: '' },
      ] },
      { caption: 'Extra keys', rows: [
        // Keyboard panning (#287): unbound by default (#307). The arrow keys are fixed and drawn by the
        // control itself; only the modifiers (one or two, required) are chosen. The switch (panKeysOn)
        // frees the keys without losing the binding.
        { key: '__pan', type: 'keybind', label: 'Pan with the arrow keys',
          desc: 'Hold the modifiers and press an arrow key to move the view.',
          panArrows: true, onKey: 'panKeysOn',
          panKeys: ['panLeftVk', 'panUpVk', 'panRightVk', 'panDownVk', 'panLeftMods', 'panUpMods', 'panRightMods', 'panDownMods'] },
        { key: '__hideCursor', type: 'keybind', label: 'Hide pointer', desc: 'Hides or shows the pointer while zoomed.',
          vkKey: 'hideCursorVk', modsKey: 'hideCursorMods', onKey: 'hideCursorOn' },
        { key: '__cursorLock', type: 'keybind', label: 'Inspect mode', desc: 'Freezes the pointer so you can look around.',
          vkKey: 'cursorLockVk', modsKey: 'cursorLockMods', onKey: 'cursorLockOn' },
      ] },
    ] },

  { id: 'zoom', label: 'Zoom', icon: 'zoom',
    desc: 'How far and how fast to zoom.',
    cards: [
      { caption: 'Level and speed', rows: [
        { key: 'maxLevel', type: 'slider', label: 'Max zoom', desc: 'The highest zoom level.', min: 2, max: 50, step: 1, def: 12.0, unit: 'times' },
        { key: 'zoomInSpeed', type: 'slider', label: 'Zoom-in speed', desc: 'How fast the view zooms in.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'zoomOutSpeed', type: 'slider', label: 'Zoom-out speed', desc: 'How fast the view zooms out.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'zoomEaseOutMs', type: 'slider', label: 'Release glide', desc: 'How long the zoom coasts after you let go.', min: 0, max: 300, step: 5, def: 45, unit: 'ms' },
      ] },
      { caption: 'Easing', rows: [
        // Smooth zoom is always on (core default 1); its shape sliders are advanced.
        { key: 'smoothZoomAccel', type: 'slider', adv: true, label: 'Soft start', desc: 'Softens the start of each zoom.', min: 1, max: 8, step: 0.5, def: 3.0 },
        { key: 'smoothZoomRamp', type: 'slider', adv: true, label: 'Soft start length', desc: 'How long the soft start lasts.', min: 0.1, max: 3, step: 0.1, def: 0.6, unit: 'seconds' },
      ] },
      { caption: 'Engine', rows: [
        { key: 'model', type: 'engine', adv: true, label: 'Engine', desc: 'How the screen is magnified.',
          options: ['hybrid', 'render', 'transform'],
          optionLabels: { hybrid: 'Auto', render: 'Render', transform: 'Transform' },
          def: 'hybrid' },
        // PER-WINDOW-TYPE ENGINE (2026-08-24). Every row defaults to Auto so an untouched install
        // behaves as before. They only apply when the engine above is Auto, so showIf hides them
        // when a single engine is pinned. ("Never use Render for" left Settings in #318; the
        // renderExclude ini key and the core's own detection stay.)
        { key: 'engineGame', type: 'select', adv: true, label: 'Engine for games', desc: 'Full-screen and borderless games.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineAcrylic', type: 'select', adv: true, label: 'Engine for blurred windows',
          desc: 'Windows with see-through, blurred backgrounds.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineDesktop', type: 'select', adv: true, label: 'Engine for the desktop',
          desc: 'The desktop, the taskbar and file windows.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineOther', type: 'select', adv: true, label: 'Engine for other windows', desc: 'All other windows.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
      ] },
    ] },

  { id: 'view', label: 'View', icon: 'view',
    desc: 'How the view follows the mouse and typing.',
    cards: [
      { caption: 'Speed', rows: [
        { key: 'panSpeed', type: 'slider', label: 'Arrow key speed', desc: 'How fast the arrow keys move the view.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'cursorSensitivity', type: 'slider', label: 'Mouse speed', desc: 'How fast the view follows the mouse.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'cursorSmoothing', type: 'slider', adv: true, label: 'Pan smoothing', desc: 'Adds gentle inertia when the view moves.', min: 0, max: 0.95, step: 0.05, def: 0.4 },
      ] },
      { caption: 'Pointer', rows: [
        { key: 'mouseAlign', type: 'select', label: 'Pointer position', desc: 'Where the mouse pointer sits while the view moves.', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
        { key: 'mouseMarginPct', type: 'slider', label: 'Edge margin', desc: 'How near the edge the pointer gets before the view moves.', min: 0, max: 30, step: 1, def: 0, unit: '%',
          showIf: { key: 'mouseAlign', eq: '1' } },
        // High resolution cursor (#227) + MPO, ONE option (#242). Ini key stays txSamplingMode
        // (0 nearest / 1 smooth); the MPO half lives in HKLM and is staged by the page. The two are
        // safety-coupled: crisp with MPO enabled is the NVIDIA 16-bit overflow TDR combo (#148), so
        // turning high-res OFF also stages MPO-disable and turning it ON stages MPO re-enable, both
        // atomic at the Windows restart (the core holds the boot state's look until then).
        { key: 'txSamplingMode', type: 'highres', label: 'High resolution cursor',
          desc: 'A sharper pointer and image at high zoom.', def: 0 },
        // Zoom lock detection (#221): games like DOOM pin the mouse to the screen centre, which would
        // pin the zoom view there too. Listed apps get the view UNLOCKED from the pointer.
        { key: 'lockApps', type: 'applist', adv: true, label: 'Mouse-locked games',
          desc: 'The view follows mouse movement in these games.', def: '' },
      ] },
      { caption: 'Typing and focus', rows: [
        { key: 'trackCaret', type: 'toggle', label: 'Follow the text cursor', desc: 'The view moves to where you type.', def: 1 },
        { key: 'trackFocus', type: 'toggle', label: 'Follow keyboard focus', desc: 'The view moves to the selected control.', def: 0 },
        { key: 'trackAlign', type: 'select', label: 'Text cursor position', desc: 'Where the text cursor and focus sit in the view.', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
      ] },
    ] },

  // Screen (#288): warmth and brightness, one DWM colour matrix (the render engine applies it in its shader).
  { id: 'screen', label: 'Screen', icon: 'screen',
    desc: 'Warmth and brightness of the screen.',
    cards: [
      { caption: 'Screen light', rows: [
        { key: 'colorWarmPct', type: 'slider', label: 'Warmth', desc: 'Makes the screen warmer.', min: 0, max: 100, step: 5, def: 0, unit: '%' },
        { key: 'colorDimPct', type: 'slider', label: 'Brightness', desc: 'Dims the whole screen.', min: 1, max: 100, step: 1, def: 100, unit: '%' },
      ] },
    ] },

  { id: 'prefs', label: 'Preferences', icon: 'general', bottom: true,
    desc: 'Appearance, profiles and troubleshooting.',
    cards: [
      { caption: 'General', rows: [
        // Global UI-only key (uiPalette): the built-in theme, shared by Settings and the tray menu. One row of mini window cards, right-aligned like the other controls.
        { key: 'uiPalette', type: 'palette', label: 'Theme', desc: 'Set your preferred look.', def: 'grey' },
        { key: '__profiles', type: 'profiles', label: 'Profile', desc: 'A saved set of all settings.' },
        // Global UI-only key: shows the advanced rows of every page.
        { key: 'showAdvanced', type: 'toggle', label: 'Show advanced settings', desc: 'Shows extra options on every page.', def: 0 },
      ] },
      { caption: 'Troubleshooting', rows: [
        // Always visible: these are not advanced settings.
        { key: 'diagnostics', type: 'toggle', label: 'Frame time logging', desc: 'Writes frame times to a log file.', def: 0 },
        { key: '__diagnostics', type: 'button', label: 'Export diagnostics', desc: 'Saves a report to share when asking for help.', btn: 'Export', action: 'exportDiagnostics' },
        { key: '__openIni', type: 'button', label: 'Open settings file', desc: 'Opens the settings file in a text editor.', btn: 'Open', action: 'openIni' },
      ] },
    ] },

  // Tray menu (#313): the Performance switch is an ordinary row; the two item lists are drawn by
  // src/tray/TrayMenuPage.svelte (custom: 'tray'). All five tray keys are global, never profile keys.
  { id: 'tray', label: 'Tray menu', icon: 'tray', custom: 'tray', bottom: true,
    desc: 'Choose what the tray menu shows, and in what order.',
    cards: [
      { caption: '', rows: [
        { key: 'trayPerf', type: 'toggle', label: 'Performance in the tray', desc: 'Zoom, fps and a frame-time graph.', def: 1 },
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

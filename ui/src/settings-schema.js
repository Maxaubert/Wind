// Settings page layout, task-based (redesign #303, spec 2026-10-01). Eight groups; each group has a
// label, a sidebar icon (src/design/icons.js), a one-line description for its banner and cards of
// rows. Every ini key from the old layout keeps its name and meaning. The one UI-only row that went
// away is "Show advanced settings": Advanced is always a group now, and the showAdvanced key stays
// parsed by the core and ignored by the UI.
//
// Row types: keybind, slider, toggle, select, applist, highres, engine (the Magnifier engine
// select with the inline "Restart Wind"), theme, profiles, button, about. A row may carry
// showIf { key, eq } to hide it unless another setting has that value (per-window engines only
// matter when the engine is Auto, i.e. 'hybrid').
//
// Copy rules from the 2026-08-21 pass still hold: plain language, no toggle labels starting with
// "Enable", no description that restates its label.

const hybrid = { key: 'model', eq: 'hybrid' };
const engineOpts = ['auto', 'transform', 'render'];
const engineLabels = { auto: 'Auto', transform: 'Transform', render: 'Render' };

export const groups = [
  { id: 'zoom', label: 'Zoom', icon: 'zoom',
    desc: 'Keys, limits and speed for magnifying the screen.',
    cards: [
      { caption: 'Keys', rows: [
        // One row per direction with TWO capture slots: the *2 keys feed the second keycap, either
        // slot works alone, both fire the same action (the core OR-combines them).
        { key: '__zoomIn', type: 'keybind', label: 'Zoom in',
          desc: 'Hold to magnify the view.',
          buttonKey: 'zoomInButton', vkKey: 'zoomInVk', modsKey: 'zoomInMods', buttonModsKey: 'zoomInButtonMods',
          buttonKey2: 'zoomInButton2', vkKey2: 'zoomInVk2', modsKey2: 'zoomInMods2', buttonModsKey2: 'zoomInButton2Mods' },
        { key: '__zoomOut', type: 'keybind', label: 'Zoom out', desc: 'Hold to return the view.',
          buttonKey: 'zoomOutButton', vkKey: 'zoomOutVk', modsKey: 'zoomOutMods', buttonModsKey: 'zoomOutButtonMods',
          buttonKey2: 'zoomOutButton2', vkKey2: 'zoomOutVk2', modsKey2: 'zoomOutMods2', buttonModsKey2: 'zoomOutButton2Mods' },
        // Scroll-wheel zoom (#285): the modifiers held while turning the wheel; up = in, down = out.
        { key: '__zoomWheel', type: 'keybind', label: 'Zoom with the scroll wheel',
          desc: 'Hold these keys and turn the wheel.',
          wheel: true, modsKey: 'zoomWheelMods' },
      ] },
      { caption: 'Levels', rows: [
        { key: 'maxLevel', type: 'slider', label: 'Max zoom', desc: 'The highest magnification you can reach.', min: 2, max: 50, step: 1, def: 12.0, unit: 'times' },
      ] },
      { caption: 'Speed', rows: [
        { key: 'zoomInSpeed', type: 'slider', label: 'Zoom-in speed', desc: 'How quickly the view magnifies while you hold the key.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'zoomOutSpeed', type: 'slider', label: 'Zoom-out speed', desc: 'How quickly the view returns while you hold the key.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
      ] },
    ] },

  { id: 'move', label: 'Moving around', icon: 'move',
    desc: 'Pan the zoomed view with the keyboard and set how the pointer travels.',
    cards: [
      { caption: 'Keys', rows: [
        // Keyboard panning (#287): move the zoomed view without the mouse. Unbound by default (#307);
        // Ctrl+Alt+arrows matches Windows Magnifier. The keys reach apps normally at 1x.
        // The arrow keys are fixed and drawn by the control itself; only the modifiers are chosen.
        { key: '__pan', type: 'keybind', label: 'Pan with the arrow keys',
          desc: 'While zoomed: tap to nudge the view, hold to pan. Off until you set modifiers (Windows Magnifier uses Ctrl+Alt). The keys work normally in apps when not zoomed.',
          panArrows: true,
          panKeys: ['panLeftVk', 'panUpVk', 'panRightVk', 'panDownVk', 'panLeftMods', 'panUpMods', 'panRightMods', 'panDownMods'] },
      ] },
      { caption: 'Panning', rows: [
        { key: 'panSpeed', type: 'slider', label: 'Pan speed', desc: 'How fast holding a pan key moves the view.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'mouseAlign', type: 'select', label: 'Keep the mouse pointer', desc: 'Within the edges: the pointer moves freely and the view only moves when it nears the edge.', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
        { key: 'mouseMarginPct', type: 'slider', label: 'Mouse edge margin', desc: 'With the pointer kept within the edges: how close it gets to the edge of the view before the view moves.', min: 0, max: 30, step: 1, def: 0, unit: '%' },
      ] },
    ] },

  { id: 'cursor', label: 'Cursor', icon: 'cursor',
    desc: 'How the pointer looks and behaves while zoomed.',
    cards: [
      { caption: 'Look', rows: [
        // High resolution cursor (#227) + MPO, ONE option (#242). Ini key stays txSamplingMode
        // (0 nearest / 1 smooth); the MPO half lives in HKLM and is staged by the page. The two are
        // safety-coupled: crisp with MPO enabled is the NVIDIA 16-bit overflow TDR combo (#148), so
        // turning high-res OFF also stages MPO-disable and turning it ON stages MPO re-enable, both
        // atomic at the Windows restart (the core holds the boot state's look until then).
        { key: 'txSamplingMode', type: 'highres', label: 'High resolution cursor',
          desc: 'A sharper cursor and image at high zoom. Changing this needs admin and a Windows restart.',
          def: 0 },
      ] },
      { caption: 'Keys', rows: [
        { key: '__hideCursor', type: 'keybind', label: 'Hide cursor', desc: 'Hides or shows the cursor while zoomed.', vkKey: 'hideCursorVk', modsKey: 'hideCursorMods' },
        { key: '__cursorLock', type: 'keybind', label: 'Inspect mode', desc: 'Freezes the cursor so tooltips stay open, while a crosshair pans the view.', vkKey: 'cursorLockVk', modsKey: 'cursorLockMods' },
      ] },
    ] },

  { id: 'typing', label: 'Follow typing', icon: 'typing',
    desc: 'What the zoomed view follows besides the mouse.',
    cards: [
      { caption: '', rows: [
        { key: 'trackCaret', type: 'toggle', label: 'Follow the text cursor', desc: 'While you type, the view glides to the text cursor. The mouse pointer stays where it was.', def: 1 },
        { key: 'trackFocus', type: 'toggle', label: 'Follow keyboard focus', desc: 'When you move with Tab or the arrow keys, the view glides to the selected control.', def: 0 },
        { key: 'trackAlign', type: 'select', label: 'Keep the text cursor and focus', options: ['0', '1'], optionLabels: { '0': 'Centred', '1': 'Within the edges' }, def: '0' },
      ] },
    ] },

  // Colour (#288): warmth and brightness, one DWM colour matrix (the render engine applies it in its shader).
  { id: 'colour', label: 'Colour', icon: 'colour',
    desc: 'Warmth and brightness for the whole screen, zoomed or not.',
    cards: [
      { caption: '', rows: [
        { key: 'colorWarmPct', type: 'slider', label: 'Warmth', desc: 'Makes the screen more orange, like Night light. 0% is off.', min: 0, max: 100, step: 5, def: 0, unit: '%' },
        { key: 'colorDimPct', type: 'slider', label: 'Brightness', desc: 'Darkens the picture, like turning down a TV. 100% is normal.', min: 1, max: 100, step: 1, def: 100, unit: '%' },
      ] },
    ] },

  { id: 'general', label: 'General', icon: 'general',
    desc: 'Appearance, profiles and files.',
    cards: [
      { caption: 'Appearance', rows: [
        { key: '__theme', type: 'theme', label: 'Theme', desc: 'Auto follows Windows.', def: 'auto' },
      ] },
      { caption: 'Profiles', rows: [
        { key: '__profiles', type: 'profiles', label: 'Profile', desc: 'A profile keeps a full set of settings. Switch, create, rename, duplicate or delete them here.' },
      ] },
      { caption: 'Files', rows: [
        { key: '__diagnostics', type: 'button', label: 'Export diagnostics', desc: 'Saves a report to attach when you ask for help.', btn: 'Export', action: 'exportDiagnostics' },
        { key: '__openIni', type: 'button', label: 'Open settings file', desc: 'Opens magnifier.ini in your editor.', btn: 'Open', action: 'openIni' },
      ] },
    ] },

  { id: 'advanced', label: 'Advanced', icon: 'adv',
    desc: 'Engines, per-app exceptions and fine tuning.',
    cards: [
      { caption: 'Engine', rows: [
        { key: 'model', type: 'engine', label: 'Magnifier engine',
          desc: 'Auto picks the best engine for the app in front. Restart to switch.',
          options: ['hybrid', 'render', 'transform', 'magnify'],
          optionLabels: { hybrid: 'Auto', render: 'Render', transform: 'Transform', magnify: 'System' },
          def: 'hybrid' },
        // PER-WINDOW-TYPE ENGINE (2026-08-24). Every row defaults to Auto so an untouched install
        // behaves as before. They only apply when the engine above is Auto, so showIf hides them
        // when a single engine is pinned.
        { key: 'engineGame', type: 'select', label: 'Engine for games',
          desc: 'Fullscreen or borderless apps. Auto uses Transform, which stays smooth over a heavy game.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineAcrylic', type: 'select', label: 'Engine for blurred windows',
          desc: 'Windows that ask for a Mica or acrylic background. Only apps that opt in are detected.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineDesktop', type: 'select', label: 'Engine for the desktop',
          desc: 'The desktop itself, with no window in front.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
        { key: 'engineOther', type: 'select', label: 'Engine for other windows',
          desc: 'Everything else: normal app windows.',
          options: engineOpts, optionLabels: engineLabels, def: 'auto', showIf: hybrid },
      ] },
      { caption: 'Apps', rows: [
        { key: 'renderExclude', type: 'applist', label: 'Never use Render for',
          desc: 'Apps whose video is copy-protected, like Netflix or Apple TV, magnify as a black rectangle on Render. Wind detects most of them on its own; list any it misses.',
          def: '', showIf: hybrid },
        // Keyboard-hook suspension (#156): trades key-interception for smooth panning, per app.
        { key: 'noSwallowApps', type: 'applist', label: 'Pass zoom keys to these apps',
          desc: 'Fixes stuttery panning in some games. The app will also receive the key.', def: '' },
        // Zoom lock detection (#221): games like DOOM pin the mouse to the screen centre, which would
        // pin the zoom view there too. Listed apps get the view UNLOCKED from the pointer.
        { key: 'lockApps', type: 'applist', label: 'Mouse-locked games',
          desc: 'For games that hold the pointer in place. The view follows your hand movement instead.', def: '' },
      ] },
      { caption: 'Fine tuning', rows: [
        { key: 'cursorSensitivity', type: 'slider', label: 'Cursor speed', desc: 'How fast the view pans with your mouse.', min: 0.25, max: 4, step: 0.05, def: 1.0, unit: 'times' },
        { key: 'cursorSmoothing', type: 'slider', label: 'Pan smoothing', desc: 'Adds gentle inertia to panning. Render engine only.', min: 0, max: 0.95, step: 0.05, def: 0.4 },
        // Smooth zoom is always on (core default 1); its shape sliders survive as fine tuning.
        { key: 'smoothZoomAccel', type: 'slider', label: 'Zoom-in ease', desc: 'Softens the start of each zoom.', min: 1, max: 8, step: 0.5, def: 3.0 },
        { key: 'smoothZoomRamp', type: 'slider', label: 'Ease-in duration', desc: 'How long the soft start lasts.', min: 0.1, max: 3, step: 0.1, def: 0.6, unit: 'seconds' },
        { key: 'zoomEaseOutMs', type: 'slider', label: 'Release glide', desc: 'How softly the zoom coasts to a stop when you let go. 0 stops instantly.', min: 0, max: 300, step: 5, def: 45, unit: 'ms' },
        { key: 'diagnostics', type: 'toggle', label: 'Frametime logging', desc: 'Logs frame timing for debugging.', def: 0 },
      ] },
    ] },

  { id: 'about', label: 'About', icon: 'about', desc: '',
    cards: [
      { caption: '', bare: true, rows: [{ key: '__about', type: 'about' }] },
    ] },
];

// Flat row list of one group, in page order.
export const groupRows = (g) => g.cards.flatMap((c) => c.rows);
export const allRows = groups.flatMap(groupRows);

// Every ini key a row reads or writes besides its own: the keybind rows keep their real state under
// buttonKey/vkKey/modsKey (and the *2 slot), so loading and diffing must cover those too.
export const bindKeys = (r) =>
  ['buttonKey', 'vkKey', 'modsKey', 'buttonModsKey', 'buttonKey2', 'vkKey2', 'modsKey2', 'buttonModsKey2']
    .map((k) => r[k]).filter(Boolean).concat(r.panKeys || []);

// Interim view for the page that predates the redesign (Settings.svelte reads `sections` with flat
// `rows`). Rows of the new-only types are left out so that page keeps working until the
// session-flow task replaces it; nothing new should import this.
const NEW_ONLY = new Set(['theme', 'profiles', 'button']);
export const sections = groups.map((g) => ({
  id: g.id, label: g.label, icon: g.icon, desc: g.desc,
  rows: groupRows(g)
    .filter((r) => !NEW_ONLY.has(r.type))
    .map((r) => (r.type === 'engine' ? { ...r, type: 'select' } : r)),
}));

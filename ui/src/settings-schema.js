// Settings page layout. Cleaned up 2026-08-21 with Max deciding every row (issue #221 branch):
// stale/niche rows were removed from the UI (their ini keys keep working) or demoted to
// advanced. Removed outright: quick zoom (mode/modifier/hotkey), smooth-zoom toggle (always on
// now - the sliders stay, advanced), scale-cursor-with-zoom, magnifyStep, desktopTransform,
// bilinear, sharpness, brightness, hdrTonemap (auto: no-op on SDR), multiMonitor, the whole
// outline family, cursorVisibility (broken in the transform model - see the Cursor section).
// Copy pass same day: plain language (no "swallow"), no toggle labels starting with "Enable",
// no desc that restates its label, consequences kept only where they change a decision.
export const sections = [
  { id:'keybinds', label:'Keybinds', icon:'keys', desc:'Hold to zoom. Each binding takes a mouse side-button or a key. Right-click to clear.', rows: [
    // One row per direction with TWO capture slots (the 'Alternate keybinds' gate left the UI
    // 2026-08-22): the *2 keys feed the second keycap, either slot works alone, both fire the
    // same action (the core OR-combines them).
    { key:'__zoomIn',   type:'keybind', label:'Zoom in',
      buttonKey:'zoomInButton',  vkKey:'zoomInVk',  modsKey:'zoomInMods',
      buttonKey2:'zoomInButton2',  vkKey2:'zoomInVk2',  modsKey2:'zoomInMods2' },
    { key:'__zoomOut',  type:'keybind', label:'Zoom out',
      buttonKey:'zoomOutButton', vkKey:'zoomOutVk', modsKey:'zoomOutMods',
      buttonKey2:'zoomOutButton2', vkKey2:'zoomOutVk2', modsKey2:'zoomOutMods2' },
    // Keyboard-hook suspension (issue #156): trades key-interception for smooth panning, per app.
    { key:'noSwallowApps', type:'applist', label:'Pass zoom keys to these apps',
      desc:'Fixes stuttery panning in some games. The app will also receive the key.',
      def:'', advanced:true },
  ]},
  { id:'zoom', label:'Zoom', icon:'zoom', desc:'How far and how fast you zoom.', rows: [
    { key:'maxLevel',     type:'slider', label:'Max zoom',       desc:'How far you can zoom.', min:2, max:50, step:1, def:12.0, unit:'times' },
    { key:'zoomInSpeed',  type:'slider', label:'Zoom-in speed',  desc:'1 = normal speed.', min:0.25, max:4, step:0.05, def:1.0, unit:'times' },
    { key:'zoomOutSpeed', type:'slider', label:'Zoom-out speed', desc:'1 = normal speed.', min:0.25, max:4, step:0.05, def:1.0, unit:'times' },
    // Smooth zoom is always on (the toggle was removed; core default is 1). Its two shape
    // sliders survive as advanced knobs.
    { key:'smoothZoomAccel', type:'slider', label:'Zoom-in ease', desc:'Higher = gentler start.', min:1, max:8, step:0.5, def:3.0, advanced:true },
    { key:'smoothZoomRamp',  type:'slider', label:'Ease-in duration', desc:'Time until full speed.', min:0.1, max:3, step:0.1, def:0.6, advanced:true, unit:'seconds' },
  ]},
  { id:'cursor', label:'Cursor', icon:'cursor', desc:'How the pointer behaves while zoomed.', rows: [
    // High resolution cursor (issue #227): DWM's edge-preserving magnification filter - the
    // whole quality gap to native Magnifier (sharp cursor AND image at every zoom). Known
    // trade, field-settled 2026-08-22: slight shimmer while the zoom level is CHANGING (the
    // filter's own re-render; not fixable externally, WM shows it too under its notchy ease).
    // Ini key stays txSamplingMode (0 nearest default / 1 smooth). Hot; applies next zoom.
    { key:'txSamplingMode', type:'toggle', label:'High resolution cursor (experimental)',
      desc:'Sharp cursor and image at high zoom, like Windows Magnifier. May shimmer slightly while zooming in or out.',
      def:0 },
    { key:'__hideCursor', type:'keybind', label:'Hide cursor', desc:'Toggles the cursor without leaving zoom.', vkKey:'hideCursorVk', modsKey:'hideCursorMods' },
    { key:'__cursorLock', type:'keybind', label:'Inspect mode', desc:'Freezes the cursor so tooltips stay open, while a crosshair pans the view.', vkKey:'cursorLockVk' },
    // Zoom lock detection (issue #221): games like DOOM pin the mouse to the screen centre,
    // which would pin the zoom view there too. Listed apps get the view UNLOCKED from the
    // pointer - it pans from raw mouse motion instead.
    { key:'lockApps', type:'applist', label:'Zoom lock detection',
      desc:'Games that pin the mouse to the screen center (e.g. DOOM). Zoom pans from raw mouse motion there.',
      def:'', advanced:true },
    { key:'cursorSensitivity', type:'slider', label:'Cursor speed', desc:'1 = match your mouse.', min:0.25, max:4, step:0.05, def:1.0, advanced:true, unit:'times' },
    { key:'cursorSmoothing',   type:'slider', label:'Pan smoothing', desc:'Adds inertia to panning. Render engine only.', min:0, max:0.95, step:0.05, def:0.4, advanced:true },
    // (cursorVisibility left the UI 2026-08-21: in the transform model 'always' and 'auto' are
    // indistinguishable - main.cpp collapses to drawCursor = mode != 2 - and 'always' cannot
    // conjure a shape while a game hides its pointer, so only 'never' did anything, which the
    // hide-cursor hotkey already covers. Ini key still parsed.)
  ]},
  { id:'display', label:'Display', icon:'display', desc:'The engine behind the magnified view.', rows: [
    { key:'model', type:'select', label:'Magnifier engine',
      desc:'Auto picks the best engine for the app in front. Restart to switch.',
      options:['hybrid','render','transform','magnify'],
      optionLabels:{ hybrid:'Auto', render:'Render', transform:'Transform', magnify:'Windows Magnifier' },
      def:'hybrid' },
    // Not an ini setting: reflects HKLM\...\Dwm\OverlayTestMode (issue #148 TDR trigger; costs
    // transform smoothness in-game). Advanced: system-wide display setting, needs UAC.
    { key:'__mpo', type:'mpo', label:'Disable MPO',
      desc:'Multi-plane overlay can stutter or crash the display driver when zooming in games. Needs admin and a Windows restart.',
      advanced:true },
  ]},
  { id:'about', label:'About', icon:'about', desc:'', rows: [
    { key:'diagnostics', type:'toggle', label:'Frametime logging', desc:'Logs frame timing for debugging.', def:0, advanced:true },
    { key:'showAdvanced', type:'toggle', label:'Show advanced settings', def:0 },
    { key:'__about', type:'about' },
  ]},
];

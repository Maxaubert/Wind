export const sections = [
  { id:'keybinds', label:'Keybinds', icon:'keys', desc:'Hold to zoom. The primary slot accepts a mouse side-button or a key. Right-click a binding to clear it.', rows: [
    { key:'__zoomIn',   type:'keybind', label:'Zoom in',  desc:'Hold to magnify',  buttonKey:'zoomInButton',  vkKey:'zoomInVk',  modsKey:'zoomInMods' },
    { key:'__zoomOut',  type:'keybind', label:'Zoom out', desc:'Hold to zoom back', buttonKey:'zoomOutButton', vkKey:'zoomOutVk', modsKey:'zoomOutMods' },
    { key:'altKeybinds', type:'toggle', label:'Enable alternate keybinds', desc:'Adds an optional second keyboard binding per direction.', def:0 },
    { key:'__zoomIn2',  type:'keybind', label:'Zoom in (alternate)',  desc:'Optional keyboard combo', vkKey:'zoomInVk2',  modsKey:'zoomInMods2',  requires:'altKeybinds' },
    { key:'__zoomOut2', type:'keybind', label:'Zoom out (alternate)', desc:'Optional keyboard combo', vkKey:'zoomOutVk2', modsKey:'zoomOutMods2', requires:'altKeybinds' },
  ]},
  { id:'zoom', label:'Zoom', icon:'zoom', desc:'How magnification grows while you hold the zoom button.', rows: [
    { key:'zoomInSpeed',  type:'slider', label:'Zoom-in speed',  desc:'Multiplier (1.0 = default).', min:0.25, max:4, step:0.05, def:1.0 },
    { key:'zoomOutSpeed', type:'slider', label:'Zoom-out speed', desc:'Multiplier (1.0 = default).', min:0.25, max:4, step:0.05, def:1.0 },
    { key:'smoothZoom',   type:'toggle', label:'Smooth zoom',    desc:'Zoom-in eases up to your speed.', def:0 },
    { key:'smoothZoomAccel', type:'slider', label:'Smooth ease-in depth', desc:'Higher = slower start.', min:1, max:8, step:0.5, def:3.0, dependsOn:'smoothZoom' },
    { key:'smoothZoomRamp',  type:'slider', label:'Smooth ramp (s)', desc:'Seconds to reach full speed.', min:0.1, max:3, step:0.1, def:0.6, dependsOn:'smoothZoom' },
    { key:'maxLevel',     type:'slider', label:'Max zoom',       desc:'How far you can zoom.', min:2, max:50, step:1, def:8.0 },
  ]},
  { id:'cursor', label:'Cursor', icon:'cursor', desc:'Pointer movement and visibility while zoomed.', rows: [
    { key:'cursorSensitivity', type:'slider', label:'Cursor speed', desc:'Pan speed multiplier (1.0 = match your mouse).', min:0.25, max:4, step:0.05, def:1.0 },
    { key:'cursorSmoothing',   type:'slider', label:'Pan smoothing', desc:'0 = off, higher = smoother.', min:0, max:0.95, step:0.05, def:0.8 },
    { key:'cursorScaleWithZoom', type:'toggle', label:'Scale cursor with zoom', def:1 },
    { key:'cursorVisibility', type:'select', label:'Cursor visibility', options:['auto','always','never'], def:'auto' },
  ]},
  { id:'display', label:'Display', icon:'display', desc:'Image quality of the magnified view.', rows: [
    { key:'bilinear',    type:'toggle', label:'Smooth scaling', desc:'Bilinear vs crisp pixels.', def:1 },
    { key:'sharpness',   type:'slider', label:'Sharpness', desc:'Crisps the magnified image (0 = off).', min:0, max:1, step:0.05, def:0.0 },
    { key:'brightness',  type:'slider', label:'Brightness', min:0.5, max:1.5, step:0.05, def:1.0 },
    { key:'hdrTonemap',  type:'toggle', label:'HDR tonemap', desc:'HDR10 to SDR when HDR is on.', def:1 },
    { key:'multiMonitor',type:'toggle', label:'Follow cursor monitor', def:1 },
  ]},
  { id:'about', label:'About', icon:'about', desc:'', rows: [
    { key:'diagnostics', type:'toggle', label:'Frametime logging', desc:'Write timing to a log file.', def:0 },
    { key:'__about', type:'about' },
  ]},
];

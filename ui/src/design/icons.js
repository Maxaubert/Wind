// Terminal icon set from the final mockup (square caps, mitred joins), plus the few window
// glyphs the shell needs. Static literals, injected with {@html iconSvg(name)}.
export const paths = {
  zoom: '<circle cx="6.75" cy="6.75" r="4.75"/><path d="M10.25 10.25L14 14M4.75 6.75h4M6.75 4.75v4"/>',
  move: '<path d="M8 1.5v13M1.5 8h13M6 1.5h4M6 14.5h4M1.5 6v4M14.5 6v4"/>',
  cursor: '<path d="M3 1.5v11.5l3-3 2.5 5 2-1-2.5-5H12z"/>',
  typing: '<path d="M5 2.5h6M5 13.5h6M8 2.5v11"/>',
  colour: '<rect x="2.5" y="2.5" width="11" height="11"/><path d="M8 2.5h5.5v11H8z" fill="currentColor"/>',
  general: '<path d="M1.5 4.5h3M8.5 4.5h6M1.5 11.5h7M12.5 11.5h2"/><rect x="4.5" y="3" width="3" height="3"/><rect x="8.5" y="10" width="3" height="3"/>',
  adv: '<path d="M2.5 4l4 4-4 4M8 12.5h5.5"/>',
  about: '<rect x="2.5" y="2.5" width="11" height="11"/><path d="M8 7v4.5M8 4.5v1"/>',
  search: '<circle cx="6.75" cy="6.75" r="4.75"/><path d="M10.25 10.25L14 14"/>',
  // Tray menu: the sidebar glyph and the quick-control icons (same drawings as the flyout, see
  // src/tray_app/flyout_icons.h). Shown bare, without a background box.
  tray: '<path d="M2 4.5l1.5 1.5L6 3.5M8.5 4.5H14M2 11.5l1.5 1.5L6 10.5M8.5 11.5H14"/>',
  warm: '<path d="M6.5 9.2V3a1.5 1.5 0 0 1 3 0v6.2a3 3 0 1 1-3 0z"/><path d="M8 6v5.5"/>',
  bright: '<circle cx="8" cy="8" r="2.8"/><path d="M8 1.5v1.8M8 12.7v1.8M1.5 8h1.8M12.7 8h1.8M3.4 3.4l1.3 1.3M11.3 11.3l1.3 1.3M3.4 12.6l1.3-1.3M11.3 4.7l1.3-1.3"/>',
  maxz: '<circle cx="6.75" cy="6.75" r="4.75"/><path d="M10.25 10.25L14 14M4.75 6.75h4M6.75 4.75v4"/>',
  zin: '<path d="M9.5 2.5h4v4M13.5 2.5L9 7M6.5 13.5h-4v-4M2.5 13.5L7 9M2.5 4.25h3.5M4.25 2.5v3.5"/>',
  zout: '<path d="M13.5 6.5h-4v-4M9.5 6.5L14 2M2.5 9.5h4v4M6.5 9.5L2 14M2.5 4.25h3.5"/>',
  pan: '<path d="M8 1.5v13M1.5 8h13M6 1.5h4M6 14.5h4M1.5 6v4M14.5 6v4"/>',
  smooth: '<path d="M2 13C7 13 7 3 12 3h2"/>',
  glide: '<circle cx="10" cy="6.5" r="3.5"/><path d="M12 9L14.5 11.5M2 9.5a6 6 0 0 0 4 4.5"/>',
  ftc: '<path d="M5 2h6M5 14h6M8 2v12"/><path d="M1.5 6v4M14.5 6v4"/>',
  ffk: '<path d="M2 5.5V3a1 1 0 0 1 1-1h2.5M10.5 2H13a1 1 0 0 1 1 1v2.5M14 10.5V13a1 1 0 0 1-1 1h-2.5M5.5 14H3a1 1 0 0 1-1-1v-2.5"/><path d="M6 6h4v4H6z"/>',
  edges: '<path d="M2.5 2.5h11v11h-11zM6 5.5l4.5 2.8-1.9.6-.8 2z"/>',
  // Theme glyphs are round on purpose: square ones read as the window maximize button.
  auto: '<circle cx="8" cy="8" r="5.5"/><path d="M8 2.5a5.5 5.5 0 0 1 0 11z" fill="currentColor"/>',
  light: '<circle cx="8" cy="8" r="2.75"/><path d="M8 1v2M8 13v2M1 8h2M13 8h2M3 3l1.4 1.4M11.6 11.6L13 13M3 13l1.4-1.4M11.6 4.4L13 3"/>',
  dark: '<path d="M13.5 9.5A5.75 5.75 0 1 1 6.5 2.5a4.5 4.5 0 0 0 7 7z"/>',
  // Window chrome and brand.
  logo: '<path d="M2 5h8a2 2 0 1 0-2-2M2 8.5h11a2 2 0 1 1-2 2M2 12h6"/>',
  minimize: '<path d="M3 8h10"/>',
  close: '<path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/>',
};

export const iconNames = Object.keys(paths);

// Returns an <svg> string for `name`; unknown names give an empty string.
export function iconSvg(name, size) {
  const p = paths[name];
  if (!p) return '';
  const sz = size ? ` width="${size}" height="${size}"` : '';
  return `<svg class="ic" aria-hidden="true" focusable="false" viewBox="0 0 16 16"${sz}>${p}</svg>`;
}

// Terminal icon set from the final mockup (square caps, mitred joins), plus the few window
// glyphs the shell needs. Static literals, injected with {@html iconSvg(name)}.
export const paths = {
  hotkeys: '<rect x="1.5" y="3.5" width="13" height="9"/><path d="M4 6.5h1M7.5 6.5h1M11 6.5h1M5 9.5h6"/>',
  view: '<path d="M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z"/><rect x="6.5" y="6.5" width="3" height="3"/>',
  screen: '<rect x="1.5" y="2.5" width="13" height="9"/><path d="M5.5 14h5M8 11.5V14"/>',
  zoom: '<circle cx="6.75" cy="6.75" r="4.75"/><path d="M10.25 10.25L14 14M4.75 6.75h4M6.75 4.75v4"/>',
  general: '<path d="M1.5 4.5h3M8.5 4.5h6M1.5 11.5h7M12.5 11.5h2"/><rect x="4.5" y="3" width="3" height="3"/><rect x="8.5" y="10" width="3" height="3"/>',
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
  // Tray tools (#315): a chip with pins, the main-engine dropdown. Same path as flyout_icons.h.
  engine: '<path d="M4.5 4.5h7v7h-7zM6.75 6.75h2.5v2.5h-2.5z"/><path d="M6.5 1.5v3M9.5 1.5v3M6.5 11.5v3M9.5 11.5v3M1.5 6.5h3M1.5 9.5h3M11.5 6.5h3M11.5 9.5h3"/>',
  // Window chrome and brand.
  logo: '<path d="M2 5h8a2 2 0 1 0-2-2M2 8.5h11a2 2 0 1 1-2 2M2 12h6"/>',
  minimize: '<path d="M3 8h10"/>',
  maximize: '<rect x="3.5" y="3.5" width="9" height="9"/>',
  close: '<path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/>',
  // Profile list (#318): the selected tick and the delete can.
  check: '<path d="M3 8.5l3.25 3.25L13 4.75"/>',
  trash: '<path d="M2.5 4h11M6 4V2.5h4V4M4 4l.75 9.5h6.5L12 4M6.75 6.5v4.5M9.25 6.5v4.5"/>',
};

export const iconNames = Object.keys(paths);

// Returns an <svg> string for `name`; unknown names give an empty string.
export function iconSvg(name, size) {
  const p = paths[name];
  if (!p) return '';
  const sz = size ? ` width="${size}" height="${size}"` : '';
  return `<svg class="ic" aria-hidden="true" focusable="false" viewBox="0 0 16 16"${sz}>${p}</svg>`;
}

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

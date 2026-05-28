// Svelte action on the scroll container. opts: { sectionIds, onActive }.
// Click-to-scroll is done by the rail (it calls scrollToSection); this watches scroll and reports
// the active section (the last one whose top has passed the 70px band), matching the mockup.
// Bottom-of-scroll fallback: when the container is scrolled to the very bottom, force the last
// section active so the rail still activates About even when its header cannot reach the top band
// (the scroll has run out of room).
export function scrollspy(node, opts) {
  let { sectionIds, onActive } = opts;
  function onScroll() {
    let cur = sectionIds[0];
    for (const id of sectionIds) {
      const el = node.querySelector('#sec-' + id);
      if (el && el.offsetTop - node.scrollTop <= 70) cur = id;
    }
    if (node.scrollHeight - (node.scrollTop + node.clientHeight) < 4) {
      cur = sectionIds[sectionIds.length - 1];
    }
    onActive(cur);
  }
  node.addEventListener('scroll', onScroll);
  onScroll();
  return {
    update(o) { sectionIds = o.sectionIds; onActive = o.onActive; onScroll(); },
    destroy() { node.removeEventListener('scroll', onScroll); },
  };
}
// Faster, controllable scroll than the browser's default behavior:'smooth' (which is ~400-500ms in
// Chromium). 220ms ease-out feels snappy; latest call cancels any in-flight animation so rapid
// clicks between rail icons resolve to the latest target instead of fighting frame-by-frame.
let raf = null;
function smoothScrollTo(node, target, duration) {
  if (raf) cancelAnimationFrame(raf);
  const start = node.scrollTop;
  const dist = target - start;
  if (Math.abs(dist) < 1) { raf = null; return; }
  const t0 = performance.now();
  function step(now) {
    const t = Math.min(1, (now - t0) / duration);
    const eased = 1 - Math.pow(1 - t, 3);   // easeOutCubic
    node.scrollTop = start + dist * eased;
    raf = t < 1 ? requestAnimationFrame(step) : null;
  }
  raf = requestAnimationFrame(step);
}
export function scrollToSection(node, id) {
  const el = node.querySelector('#sec-' + id);
  if (el) smoothScrollTo(node, el.offsetTop - 4, 220);
}

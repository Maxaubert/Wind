// Svelte action on the scroll container. opts: { sectionIds, onActive }.
// Click-to-scroll is done by the rail (it calls scrollToSection); this watches scroll and reports
// the active section (the last one whose top has passed the 70px band), matching the mockup.
export function scrollspy(node, opts) {
  let { sectionIds, onActive } = opts;
  function onScroll() {
    let cur = sectionIds[0];
    for (const id of sectionIds) {
      const el = node.querySelector('#sec-' + id);
      if (el && el.offsetTop - node.scrollTop <= 70) cur = id;
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
export function scrollToSection(node, id) {
  const el = node.querySelector('#sec-' + id);
  if (el) node.scrollTo({ top: el.offsetTop - 4, behavior: 'smooth' });
}

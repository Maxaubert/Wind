// Modal focus management (issue #201).
//
// Every dialog in this app already carried role="dialog" aria-modal="true", which is why the
// problem was easy to miss: the markup looked right. But nothing ever moved FOCUS into the box, so
// focus stayed on the background button that opened it (measured: focusInsideDialog=false on both
// the app-list dialog and the unsaved-changes prompt). A screen reader announces a dialog when
// focus lands inside it, so "Restart to finish" / "MPO change not applied" / "Settings not applied"
// were completely silent, and Tab walked straight out into the page behind.
//
// Use on the dialog BOX (the element carrying role="dialog"), not the backdrop:
//     <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="..."
//          use:dialog={{ onClose: () => (thing = false) }}>
//
// onClose is optional. Omit it for a dialog that must be answered by pressing one of its buttons
// (Escape then does nothing, which is the correct behaviour for a prompt with no cancel path).
//
// aria-modal="true" is what keeps a screen reader's browse cursor inside the dialog; this action
// supplies the other half, the Tab loop, so keyboard-only users are held too. We deliberately do
// NOT set `inert` on the rest of the page: the app-list dialog is rendered inside the settings
// pane it would have to inert, so an inert-based trap would disable that dialog itself.
const FOCUSABLE = [
  'a[href]', 'button:not([disabled])', 'input:not([disabled])', 'select:not([disabled])',
  'textarea:not([disabled])', '[tabindex]:not([tabindex="-1"])',
].join(',');

// Mount order, so a document-level Escape only ever reaches the TOP dialog. Wind can legitimately
// stack (the app-list dialog sits inside the settings pane that a profile prompt also covers).
const stack = [];

export function dialog(node, opts = {}) {
  let options = opts || {};
  // Where focus came from, so it can be handed back when the dialog closes. Read at mount, before
  // anything is moved.
  const opener = document.activeElement;
  stack.push(node);

  const focusable = () => [...node.querySelectorAll(FOCUSABLE)]
    .filter(el => el.offsetWidth > 0 || el.offsetHeight > 0 || el === document.activeElement);

  // Focus the primary action if the dialog has one (these prompts are all "here is what happened,
  // here is the button"), else the first control, else the box.
  //
  // SYNCHRONOUSLY, not on a timeout. A Svelte action runs once its element and children exist, so
  // there is nothing to wait for - and deferring left a window where the dialog was on screen but
  // focus was still on the button behind it, so an Escape pressed straight after opening went to
  // the page instead of the dialog (caught by the existing "Escape closes the dialog" test).
  function focusIn() {
    if (!node.isConnected) return false;
    const target = node.querySelector('[data-autofocus]') ||
                   node.querySelector('button.primary') ||
                   focusable()[0];
    if (target) { target.focus(); return true; }
    node.setAttribute('tabindex', '-1');
    node.focus();
    return node.contains(document.activeElement);
  }
  // Fallback tick only for a dialog whose content is not there yet on mount.
  const t = focusIn() ? null : setTimeout(focusIn, 0);

  function onKey(e) {
    if (e.key === 'Escape') {
      if (!options.onClose) return;
      e.preventDefault();
      e.stopPropagation();   // do not also close a menu/popup underneath
      options.onClose();
      return;
    }
    if (e.key !== 'Tab') return;
    const list = focusable();
    if (list.length === 0) { e.preventDefault(); return; }
    const first = list[0], last = list[list.length - 1];
    const inside = node.contains(document.activeElement);
    if (e.shiftKey && (document.activeElement === first || !inside)) {
      e.preventDefault(); last.focus();
    } else if (!e.shiftKey && (document.activeElement === last || !inside)) {
      e.preventDefault(); first.focus();
    }
  }
  node.addEventListener('keydown', onKey);

  // Belt and braces for Escape: if anything ever leaves focus outside the box (a click on the
  // backdrop, a control that blurs itself), the node listener above would never fire. Only the
  // topmost dialog reacts, and only when focus is NOT already inside it - otherwise onKey has it.
  function onDocKey(e) {
    if (e.key !== 'Escape' || !options.onClose) return;
    if (stack[stack.length - 1] !== node) return;
    if (node.contains(document.activeElement)) return;
    e.preventDefault();
    options.onClose();
  }
  document.addEventListener('keydown', onDocKey);

  return {
    update(o) { options = o || {}; },
    destroy() {
      if (t) clearTimeout(t);
      const i = stack.indexOf(node);
      if (i >= 0) stack.splice(i, 1);
      document.removeEventListener('keydown', onDocKey);
      node.removeEventListener('keydown', onKey);
      // Hand focus back to whatever opened the dialog. Guard on isConnected: a dialog whose opener
      // was removed by the very action it confirmed (a deleted profile row) must not throw.
      if (opener && opener.isConnected && typeof opener.focus === 'function') opener.focus();
    },
  };
}

<script>
  // row.buttonKey = 'zoomInButton'|'zoomOutButton' (omit for keyboard-only slots);
  // row.vkKey = 'zoomInVk'|'zoomOutVk'|'zoomInVk2'|'zoomOutVk2'.
  // `values` is the current binding map (parent-staged). `onChange(patch)` is the LIVE setter:
  // it writes setConfig immediately AND updates the parent's display state, so a rebind becomes
  // effective the moment a key/button is captured. This matters because the magnifier core's
  // global hook swallows the bound key/button; without an immediate clear, pressing the OLD
  // bound key to re-bind it would be intercepted by the hook and never reach this UI.
  export let row, values, onChange, disabled = false;
  let armed = false;
  let preCapture = null;
  const VK_NAMES = { 33:'PageUp', 34:'PageDown', 112:'F1', 113:'F2', 145:'ScrollLock' };

  $: lbl = (function () {
    if (row.buttonKey) {
      const btn = Number(values[row.buttonKey] || 0);
      if (btn === 2) return 'Mouse button 5';
      if (btn === 1) return 'Mouse button 4';
    }
    const vk = Number(values[row.vkKey] || 0);
    if (vk) return VK_NAMES[vk] || ('Key ' + vk);
    return 'Unbound';
  })();

  // Arming: snapshot the current binding (for Escape restore) and live-clear it so the magnifier
  // core stops swallowing the previously bound key/button. Re-arming while already armed is a
  // no-op so the snapshot is not overwritten by the cleared values.
  function arm() {
    if (disabled || armed) return;
    preCapture = { [row.vkKey]: String(values[row.vkKey] ?? '0') };
    if (row.buttonKey) preCapture[row.buttonKey] = String(values[row.buttonKey] ?? '0');
    const clearPatch = { [row.vkKey]: '0' };
    if (row.buttonKey) clearPatch[row.buttonKey] = '0';
    onChange(clearPatch);
    armed = true;
  }
  function cancel() {
    if (preCapture) onChange(preCapture);
    armed = false; preCapture = null;
  }
  function onKey(e) {
    if (!armed) return;
    e.preventDefault();
    if (e.key === 'Escape') { cancel(); return; }
    if (!e.keyCode) return;   // ignore keys with no VK code (Fn modifiers, IME composition, etc.)
    const patch = { [row.vkKey]: String(e.keyCode) };
    if (row.buttonKey) patch[row.buttonKey] = '0';
    onChange(patch);
    armed = false; preCapture = null;
  }
  function onMouse(e) {
    if (!armed || !row.buttonKey) return;     // keyboard-only slot: ignore mouse
    const btn = e.button === 3 ? '1' : e.button === 4 ? '2' : null;
    if (!btn) return;
    e.preventDefault();
    onChange({ [row.buttonKey]: btn, [row.vkKey]: '0' });
    armed = false; preCapture = null;
  }
  // Right-click clears the binding (Unbound). Works whether or not the keycap is armed.
  function clear() {
    const patch = { [row.vkKey]: '0' };
    if (row.buttonKey) patch[row.buttonKey] = '0';
    onChange(patch);
    armed = false; preCapture = null;
  }
</script>
<svelte:window on:keyup={onKey} on:mousedown={onMouse} />
<button class="keycap" class:armed {disabled}
        on:click={arm}
        on:blur={() => { if (armed) cancel(); }}
        on:contextmenu|preventDefault={clear}
        title="Click to bind, right-click to clear">
  {armed ? (row.buttonKey ? 'Press a key or side-button...' : 'Press a key...') : lbl}
</button>
<style>
  /* Ported from mockups/config-ui-onboarding.html .keycap. */
  .keycap { padding: 4px 10px; border-radius: 6px; border: 1px solid var(--line); background: var(--chip); font-size: 11.5px; color: var(--text); cursor: pointer; }
  .keycap.armed { outline: 2px solid var(--accent); }
  .keycap:disabled { opacity: .5; cursor: default; }
</style>

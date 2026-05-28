<script>
  // row.buttonKey = 'zoomInButton'|'zoomOutButton' (omit for keyboard-only slots);
  // row.vkKey = 'zoomInVk'|'zoomOutVk'|'zoomInVk2'|'zoomOutVk2'.
  // `values` is the current staged map. `onChange(patch)` accepts a multi-key object so we can
  // update both sibling keys atomically (avoids the "one key behind" symptom where two sequential
  // single-key setter calls let one render see an inconsistent intermediate state).
  export let row, values, onChange, disabled = false;
  let armed = false;
  const VK_NAMES = { 33:'PageUp', 34:'PageDown', 112:'F1', 113:'F2', 145:'ScrollLock' };

  // Reactive label: $: ensures Svelte re-evaluates whenever row or values changes, which a plain
  // `label()` function call in the template did not reliably do (Svelte's static analysis cannot
  // see dependencies hidden inside a function body).
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

  // Capture on keyup (the conventional moment for rebind UIs): the key has fully resolved, and
  // keyCode === 0 events (Fn combos, IME, etc.) are skipped so they cannot wipe the binding.
  function onKey(e) {
    if (!armed) return;
    e.preventDefault();
    if (e.key === 'Escape') { armed = false; return; }
    if (!e.keyCode) return;
    const patch = { [row.vkKey]: String(e.keyCode) };
    if (row.buttonKey) patch[row.buttonKey] = '0';
    onChange(patch);
    armed = false;
  }
  function onMouse(e) {
    if (!armed || !row.buttonKey) return;     // keyboard-only slot: ignore mouse
    const btn = e.button === 3 ? '1' : e.button === 4 ? '2' : null;
    if (!btn) return;
    e.preventDefault();
    onChange({ [row.buttonKey]: btn, [row.vkKey]: '0' });
    armed = false;
  }
  // Right-click clears the binding (Unbound). Works whether or not the keycap is armed.
  function clear() {
    const patch = { [row.vkKey]: '0' };
    if (row.buttonKey) patch[row.buttonKey] = '0';
    onChange(patch);
    armed = false;
  }
</script>
<svelte:window on:keyup={onKey} on:mousedown={onMouse} />
<button class="keycap" class:armed {disabled}
        on:click={() => armed = true}
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

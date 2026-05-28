<script>
  // row.buttonKey = 'zoomInButton'|'zoomOutButton' (omit for keyboard-only slots);
  // row.vkKey = 'zoomInVk'|'zoomOutVk'|'zoomInVk2'|'zoomOutVk2'.
  // `values` holds current { [buttonKey]: '1'|'2'|'0', [vkKey]: '<vk>' }; onChange(key, val) is the
  // two-arg setter from Settings (stages the change; Apply writes it).
  export let row, values, onChange, disabled = false;
  let armed = false;
  const VK_NAMES = { 33:'PageUp', 34:'PageDown', 112:'F1', 113:'F2', 145:'ScrollLock' };
  // If row.buttonKey is missing, this is a keyboard-only slot (an alternate binding); the
  // mouse-button handler short-circuits and the label/prompt drop the "side-button" wording.
  function label() {
    if (row.buttonKey) {
      const btn = Number(values[row.buttonKey] || 0);
      if (btn === 2) return 'Mouse button 5';
      if (btn === 1) return 'Mouse button 4';
    }
    const vk = Number(values[row.vkKey] || 0);
    if (vk) return VK_NAMES[vk] || ('Key ' + vk);
    return 'Unbound';
  }
  // Capture on keyup (not keydown). Keyup is the conventional moment for rebind UIs: the key has
  // fully resolved, and keys with synthesized/0 keyCodes (Fn combos, IME, etc.) never write a
  // bogus '0' over the existing binding (which was the "one key behind" symptom: the first press
  // wrote '0' clearing it, the second press wrote the real VK).
  function onKey(e) {
    if (!armed) return;
    e.preventDefault();
    if (e.key === 'Escape') { armed = false; return; }
    if (!e.keyCode) return;   // ignore keys with no VK code (Fn modifiers, IME composition, etc.)
    // e.keyCode is the Windows Virtual-Key code the core reads from magnifier.ini (intentional;
    // e.key / e.code would need a reverse lookup). Deprecated in the DOM but stable in WebView2.
    onChange(row.vkKey, String(e.keyCode));
    if (row.buttonKey) onChange(row.buttonKey, '0');
    armed = false;
  }
  function onMouse(e) {
    if (!armed || !row.buttonKey) return;     // keyboard-only slot: ignore mouse
    if (e.button === 3) { onChange(row.buttonKey, '1'); onChange(row.vkKey, '0'); e.preventDefault(); armed = false; }
    else if (e.button === 4) { onChange(row.buttonKey, '2'); onChange(row.vkKey, '0'); e.preventDefault(); armed = false; }
  }
  // Right-click clears the binding (Unbound). Works whether or not the keycap is armed.
  function clear() {
    onChange(row.vkKey, '0');
    if (row.buttonKey) onChange(row.buttonKey, '0');
    armed = false;
  }
</script>
<svelte:window on:keyup={onKey} on:mousedown={onMouse} />
<button class="keycap" class:armed {disabled}
        on:click={() => armed = true}
        on:contextmenu|preventDefault={clear}
        title="Click to bind, right-click to clear">
  {armed ? (row.buttonKey ? 'Press a key or side-button...' : 'Press a key...') : label()}
</button>
<style>
  /* Ported from mockups/config-ui-onboarding.html .keycap. */
  .keycap { padding: 4px 10px; border-radius: 6px; border: 1px solid var(--line); background: var(--chip); font-size: 11.5px; color: var(--text); cursor: pointer; }
  .keycap.armed { outline: 2px solid var(--accent); }
  .keycap:disabled { opacity: .5; cursor: default; }
</style>

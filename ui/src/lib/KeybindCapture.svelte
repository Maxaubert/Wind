<script>
  // row.buttonKey = 'zoomInButton'|'zoomOutButton'; row.vkKey = 'zoomInVk'|'zoomOutVk'.
  // `values` holds current { [buttonKey]: '1'|'2'|'0', [vkKey]: '<vk>' }; onChange(key, val) is the
  // two-arg setter from Settings (stages the change; Apply writes it).
  export let row, values, onChange, disabled = false;
  let armed = false;
  const VK_NAMES = { 33:'PageUp', 34:'PageDown', 112:'F1', 113:'F2', 145:'ScrollLock' };
  function label() {
    const btn = Number(values[row.buttonKey] || 0);
    if (btn === 2) return 'Mouse button 5';
    if (btn === 1) return 'Mouse button 4';
    const vk = Number(values[row.vkKey] || 0);
    if (vk) return VK_NAMES[vk] || ('Key ' + vk);
    return 'Unbound';
  }
  function onKey(e) {
    if (!armed) return;
    e.preventDefault();
    if (e.key === 'Escape') { armed = false; return; }   // cancel capture, do not bind Escape
    // e.keyCode is the Windows Virtual-Key code the core reads from magnifier.ini (intentional;
    // e.key / e.code would need a reverse lookup). Deprecated in the DOM but stable in WebView2.
    onChange(row.vkKey, String(e.keyCode));
    onChange(row.buttonKey, '0');
    armed = false;
  }
  function onMouse(e) {
    if (!armed) return;
    if (e.button === 3) { onChange(row.buttonKey, '1'); onChange(row.vkKey, '0'); e.preventDefault(); armed = false; }
    else if (e.button === 4) { onChange(row.buttonKey, '2'); onChange(row.vkKey, '0'); e.preventDefault(); armed = false; }
  }
</script>
<svelte:window on:keydown={onKey} on:mousedown={onMouse} />
<button class="keycap" class:armed {disabled} on:click={() => armed = true}>
  {armed ? 'Press a key or side-button...' : label()}
</button>
<style>
  /* Ported from mockups/config-ui-onboarding.html .keycap. */
  .keycap { padding: 4px 10px; border-radius: 6px; border: 1px solid var(--line); background: var(--chip); font-size: 11.5px; color: var(--text); cursor: pointer; }
  .keycap.armed { outline: 2px solid var(--accent); }
  .keycap:disabled { opacity: .5; cursor: default; }
</style>

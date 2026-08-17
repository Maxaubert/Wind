<script>
  // row.buttonKey = 'zoomInButton'|'zoomOutButton' (omit for keyboard-only slots);
  // row.vkKey = main VK ('zoomInVk' etc.); row.modsKey = modifier mask ('zoomInMods' etc., optional).
  // `values` is the current binding map (parent-staged). `onChange(patch)` is the LIVE setter:
  // it writes setConfig immediately AND updates the parent's display state, so a rebind becomes
  // effective the moment a key/button is captured. This matters because the magnifier core's
  // global hook swallows the bound key/button; without an immediate clear, pressing the OLD
  // bound key to re-bind it would be intercepted by the hook and never reach this UI.
  export let row, values, onChange, disabled = false;
  // Naming from the owning Row (issue #201): `labelledby` lists the row label AND the value span,
  // so the keycap reads "Zoom in, Mouse button 5 + PageUp" instead of a bare binding with no
  // indication of which slot it belongs to.
  export let labelledby = undefined, describedby = undefined, valueId = undefined;
  let armed = false;
  let preCapture = null;
  // The armed state used to be conveyed by the visible label alone, with the instructions in a
  // `title` tooltip - which is never announced on keyboard focus. This region speaks it.
  let liveMsg = '';
  const uid = 'kc-' + Math.random().toString(36).slice(2, 8);

  // VK -> readable name. Covers the common cases; falls back to "Key N" for the rest.
  function vkName(vk) {
    const SPECIAL = {
      8:'Backspace', 9:'Tab', 13:'Enter', 27:'Esc', 32:'Space',
      33:'PageUp', 34:'PageDown', 35:'End', 36:'Home',
      37:'Left', 38:'Up', 39:'Right', 40:'Down',
      45:'Insert', 46:'Delete', 91:'LWin', 92:'RWin',
      96:'Num0', 97:'Num1', 98:'Num2', 99:'Num3', 100:'Num4',
      101:'Num5', 102:'Num6', 103:'Num7', 104:'Num8', 105:'Num9',
      106:'Num*', 107:'Num+', 109:'Num-', 110:'Num.', 111:'Num/',
      144:'NumLock', 145:'ScrollLock',
      186:';', 187:'=', 188:',', 189:'-', 190:'.', 191:'/', 192:'`',
      219:'[', 220:'\\', 221:']', 222:"'",
    };
    if (SPECIAL[vk]) return SPECIAL[vk];
    if (vk >= 112 && vk <= 123) return 'F' + (vk - 111);
    if ((vk >= 48 && vk <= 57) || (vk >= 65 && vk <= 90)) return String.fromCharCode(vk);
    return 'Key ' + vk;
  }
  const MOD_BITS = [ {bit:1, name:'Ctrl'}, {bit:2, name:'Alt'}, {bit:4, name:'Shift'}, {bit:8, name:'Win'} ];
  function modsName(mods) { return MOD_BITS.filter(m => mods & m.bit).map(m => m.name).join('+'); }
  function comboName(mods, vk) {
    const m = modsName(mods);
    const v = vk ? vkName(vk) : '';
    return [m, v].filter(Boolean).join('+');
  }

  // A slot can hold a side-button AND a key at once, and the core OR-combines them (main.cpp:
  // inHeld = button || comboHeld(vk...)), so both genuinely fire. The label must therefore list
  // EVERY live binding rather than returning on the first one found: a binding the label hides is
  // one the user can neither see nor clear. That is exactly how a stale zoomInVk=33/zoomOutVk=34
  // kept PageUp/PageDown zooming while the row read "Mouse button 5" (right-click clears both).
  $: lbl = (function () {
    const parts = [];
    if (row.buttonKey) {
      const btn = Number(values[row.buttonKey] || 0);
      if (btn === 2) parts.push('Mouse button 5');
      else if (btn === 1) parts.push('Mouse button 4');
    }
    const vk = Number(values[row.vkKey] || 0);
    const mods = row.modsKey ? Number(values[row.modsKey] || 0) : 0;
    if (vk || mods) {
      const combo = comboName(mods, vk);
      if (combo) parts.push(combo);
    }
    return parts.join(' + ') || 'Unbound';
  })();

  // Arming: snapshot the current binding (for Escape restore) and live-clear it so the magnifier
  // core stops swallowing the previously bound key/button. Re-arming while already armed is a
  // no-op so the snapshot is not overwritten by the cleared values.
  function arm() {
    if (disabled || armed) return;
    liveMsg = row.buttonKey
      ? 'Listening. Press a key, a combination, or a mouse side-button. Escape cancels, Tab leaves.'
      : 'Listening. Press a key or a combination. Escape cancels, Tab leaves.';
    preCapture = { [row.vkKey]: String(values[row.vkKey] ?? '0') };
    if (row.buttonKey) preCapture[row.buttonKey] = String(values[row.buttonKey] ?? '0');
    if (row.modsKey)   preCapture[row.modsKey]   = String(values[row.modsKey]   ?? '0');
    const clearPatch = { [row.vkKey]: '0' };
    if (row.buttonKey) clearPatch[row.buttonKey] = '0';
    if (row.modsKey)   clearPatch[row.modsKey]   = '0';
    onChange(clearPatch);
    armed = true;
  }
  function cancel() {
    if (preCapture) onChange(preCapture);
    armed = false; preCapture = null;
    liveMsg = 'Cancelled. Binding unchanged.';
  }
  // Capture on keydown so a combo (Ctrl+Alt+F1) is captured the instant the main key fires while
  // all modifiers are held. Modifier-only presses (Ctrl/Alt/Shift/Win) are skipped so we wait for
  // the main key. keyCode 0 is ignored (Fn/IME/synthesized events would otherwise clear the binding).
  // VKs the core refuses to bind (mirrors IsForbiddenBindVk in config.cpp): a bound key is swallowed
  // system-wide, so binding one of these would make the user lose it everywhere. Left/right click
  // (1/2) can't arrive here as a keyCode and the Win keys (91/92) are skipped below as modifier-only,
  // but we list them all so the rule is explicit and Backspace (8) is rejected. Stay armed so the
  // user can press a different key.
  const FORBIDDEN_VK = new Set([1, 2, 8, 91, 92]);
  function onKey(e) {
    if (!armed) return;
    if (e.key === 'Escape') { e.preventDefault(); cancel(); return; }
    // Tab must stay a navigation key. It is bindable in principle (keyCode 9 is not forbidden), so
    // capturing it meant a keyboard user who armed a row had no way out except Escape - which is
    // undiscoverable. Let the browser move focus; the blur handler below cancels the capture.
    if (e.key === 'Tab') return;
    e.preventDefault();
    if (!e.keyCode) return;
    if (e.keyCode === 16 || e.keyCode === 17 || e.keyCode === 18 || e.keyCode === 91 || e.keyCode === 92) return;
    if (FORBIDDEN_VK.has(e.keyCode)) return;   // can't bind a key Wind must never swallow
    const mods = (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);
    const patch = { [row.vkKey]: String(e.keyCode) };
    if (row.modsKey)   patch[row.modsKey]   = String(mods);
    if (row.buttonKey) patch[row.buttonKey] = '0';
    onChange(patch);
    armed = false; preCapture = null;
    liveMsg = 'Bound to ' + (comboName(mods, e.keyCode) || vkName(e.keyCode));
  }
  function onMouse(e) {
    if (!armed || !row.buttonKey) return;     // keyboard-only slot: ignore mouse
    const btn = e.button === 3 ? '1' : e.button === 4 ? '2' : null;
    if (!btn) return;
    e.preventDefault();
    const patch = { [row.buttonKey]: btn, [row.vkKey]: '0' };
    if (row.modsKey) patch[row.modsKey] = '0';
    onChange(patch);
    armed = false; preCapture = null;
    liveMsg = 'Bound to mouse button ' + (btn === '2' ? '5' : '4');
  }
  // Right-click clears the binding (Unbound). Works whether or not the keycap is armed.
  function clear() {
    const patch = { [row.vkKey]: '0' };
    if (row.buttonKey) patch[row.buttonKey] = '0';
    if (row.modsKey)   patch[row.modsKey]   = '0';
    onChange(patch);
    armed = false; preCapture = null;
    liveMsg = 'Binding cleared. Unbound.';
  }
</script>
<svelte:window on:keydown={onKey} on:mousedown={onMouse} />
<!-- The instructions were `title`-only, which a screen reader never reads on keyboard focus.
     They are a real description now, appended to the row's own. -->
<button class="keycap" type="button" class:armed {disabled} id={valueId}
        aria-labelledby={labelledby} aria-describedby="{describedby ?? ''} {uid}-hint"
        on:click={arm}
        on:blur={() => { if (armed) cancel(); }}
        on:contextmenu|preventDefault={clear}
        title="Click to bind (combos like Ctrl+Alt+F1 work), right-click to clear">
  {armed ? (row.buttonKey ? 'Press a key, combo, or side-button...' : 'Press a key or combo...') : lbl}
</button>
<span class="sr-only" id="{uid}-hint" aria-hidden="true">
  Activate to rebind{row.buttonKey ? ', then press a key, a combination, or a mouse side-button' : ', then press a key or a combination'}. Escape cancels. Right-click, or use the context-menu key, to clear the binding.
</span>
<span class="sr-only" role="status" aria-live="assertive">{liveMsg}</span>
<style>
  /* Ported from mockups/config-ui-onboarding.html .keycap. */
  .keycap { padding: 4px 10px; border-radius: 6px; border: 1px solid var(--line); background: var(--chip); font-size: 11.5px; color: var(--text); cursor: pointer; }
  .keycap.armed { outline: 2px solid var(--accent); }
  .keycap:disabled { opacity: .5; cursor: default; }
</style>

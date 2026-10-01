<script>
  // row.buttonKey = 'zoomInButton'|'zoomOutButton' (omit for keyboard-only slots);
  // row.vkKey = main VK ('zoomInVk' etc.); row.modsKey = modifier mask ('zoomInMods' etc., optional).
  // `values` is the current binding map (parent-staged). `onChange(patch)` is the LIVE setter:
  // it writes setConfig immediately AND updates the parent's display state, so a rebind becomes
  // effective the moment a key/button is captured. This matters because the magnifier core's
  // global hook swallows the bound key/button; without an immediate clear, pressing the OLD
  // bound key to re-bind it would be intercepted by the hook and never reach this UI.
  // #285: row.buttonModsKey = modifier mask for the button slot (left/right/middle click need one);
  // row.wheel = a wheel row (modsKey = zoomWheelMods only). Every capture goes through the shared
  // safety rules (keybindRules.js, mirrored from src/keybind_rules.h); a refused press keeps the row
  // listening and says why, on screen and to screen readers.
  import { checkKeyBind, checkClickBind, checkWheelBind, refusalText } from './keybindRules.js';
  export let row, values, onChange, disabled = false;
  // What an empty slot says. The redesigned page passes 'Add key' (a soft chip); the default keeps
  // the wording the onboarding and older tests rely on.
  export let unboundText = 'Unbound';
  // Settings page: show a combo as one keycap per key with a '+' between them, and the short
  // 'Mouse 5' names. Off by default so the onboarding keeps its single-chip label.
  export let split = false;
  const SHORT_BUTTON = { 1: 'Mouse 4', 2: 'Mouse 5' };
  let refusal = '';
  // A click captured ON the keycap is followed by its own click event, which would re-arm the row
  // and clear the bind just made: ignore an arm that close after a mouse capture.
  let mouseCapturedAt = -Infinity;
  // A right-click WITH modifiers is a bind attempt (captured or refused on mousedown); the
  // contextmenu event that follows it must not then clear the row.
  let skipContextMenu = false;
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
  const BUTTON_NAMES = { 1:'Mouse button 4', 2:'Mouse button 5', 3:'Left click', 4:'Right click', 5:'Middle click' };
  const eventMods = e => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);
  function refuse(verdict, what) {
    refusal = refusalText(verdict, what);
    liveMsg = refusal;
  }
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
    if (row.wheel) {
      const wm = Number(values[row.modsKey] || 0);
      return wm ? modsName(wm) + '+Wheel' : null;
    }
    if (row.buttonKey) {
      const btn = Number(values[row.buttonKey] || 0);
      const bm = row.buttonModsKey ? Number(values[row.buttonModsKey] || 0) : 0;
      const name = BUTTON_NAMES[btn];
      if (name) parts.push([modsName(bm), name].filter(Boolean).join('+'));
    }
    const vk = Number(values[row.vkKey] || 0);
    const mods = row.modsKey ? Number(values[row.modsKey] || 0) : 0;
    if (vk || mods) {
      const combo = comboName(mods, vk);
      if (combo) parts.push(combo);
    }
    return parts.join(' + ') || null;
  })();

  // The same live bindings as individual key names, for the split (one cap per key) display.
  $: caps = (function () {
    const out = [];
    const modList = (m) => MOD_BITS.filter(b => m & b.bit).map(b => b.name);
    if (row.wheel) {
      const wm = Number(values[row.modsKey] || 0);
      return wm ? [...modList(wm), 'Wheel'] : [];
    }
    if (row.buttonKey) {
      const btn = Number(values[row.buttonKey] || 0);
      const bm = row.buttonModsKey ? Number(values[row.buttonModsKey] || 0) : 0;
      const name = SHORT_BUTTON[btn] || BUTTON_NAMES[btn];
      if (name) out.push(...modList(bm), name);
    }
    const vk = Number(values[row.vkKey] || 0);
    const mods = row.modsKey ? Number(values[row.modsKey] || 0) : 0;
    if (vk || mods) out.push(...modList(mods), ...(vk ? [vkName(vk)] : []));
    return out;
  })();

  // Arming: snapshot the current binding (for Escape restore) and live-clear it so the magnifier
  // core stops swallowing the previously bound key/button. Re-arming while already armed is a
  // no-op so the snapshot is not overwritten by the cleared values.
  function arm() {
    if (disabled || armed) return;
    if (performance.now() - mouseCapturedAt < 500) return;
    refusal = '';
    liveMsg = row.wheel
      ? 'Listening. Hold modifier keys and turn the mouse wheel. Escape cancels, Tab leaves.'
      : row.buttonKey
        ? 'Listening. Press a key, a combination, a mouse side-button, or a click with modifiers. Escape cancels, Tab leaves.'
        : 'Listening. Press a key or a combination. Escape cancels, Tab leaves.';
    preCapture = {};
    const clearPatch = {};
    for (const k of [row.vkKey, row.buttonKey, row.modsKey, row.buttonModsKey]) {
      if (!k) continue;
      preCapture[k] = String(values[k] ?? '0');
      clearPatch[k] = '0';
    }
    onChange(clearPatch);
    armed = true;
  }
  function cancel() {
    if (preCapture) onChange(preCapture);
    armed = false; preCapture = null; refusal = '';
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
    if (row.wheel) return;                     // the wheel row binds the wheel, not a key
    const mods = eventMods(e);
    if (FORBIDDEN_VK.has(e.keyCode)) { refuse('never', vkName(e.keyCode)); return; }
    // Rows without a modifier slot (Inspect) bind the bare key; judge it as one.
    const verdict = checkKeyBind(e.keyCode, row.modsKey ? mods : 0);
    if (verdict !== 'ok') { refuse(verdict, row.modsKey ? comboName(mods, e.keyCode) : vkName(e.keyCode)); return; }
    const patch = { [row.vkKey]: String(e.keyCode) };
    if (row.modsKey)   patch[row.modsKey]   = String(mods);
    if (row.buttonKey) patch[row.buttonKey] = '0';
    if (row.buttonModsKey) patch[row.buttonModsKey] = '0';
    onChange(patch);
    armed = false; preCapture = null; refusal = '';
    liveMsg = 'Bound to ' + (comboName(mods, e.keyCode) || vkName(e.keyCode));
  }
  function onMouse(e) {
    if (!armed || !row.buttonKey || row.wheel) return;   // keyboard-only slot: ignore mouse
    // DOM buttons: 0 left, 1 middle, 2 right, 3 back, 4 forward -> slot ids 3, 5, 4, 1, 2.
    const btn = { 0: 3, 1: 5, 2: 4, 3: 1, 4: 2 }[e.button];
    if (!btn) return;
    const mods = eventMods(e);
    if (btn === 4 && mods === 0) return;        // a plain right-click clears the row (contextmenu)
    e.preventDefault();
    if (btn === 4) skipContextMenu = true;
    const what = [modsName(mods), BUTTON_NAMES[btn]].filter(Boolean).join('+');
    const verdict = checkClickBind(btn, mods);
    if (verdict !== 'ok') { refuse(verdict, what); return; }
    const patch = { [row.buttonKey]: String(btn), [row.vkKey]: '0' };
    if (row.modsKey) patch[row.modsKey] = '0';
    // Side buttons keep their modifiers too (Ctrl+Mouse4), like the clicks.
    if (row.buttonModsKey) patch[row.buttonModsKey] = String(mods);
    mouseCapturedAt = performance.now();
    onChange(patch);
    armed = false; preCapture = null; refusal = '';
    liveMsg = 'Bound to ' + (row.buttonModsKey ? what : BUTTON_NAMES[btn]);
  }
  // The wheel row: the modifiers held while the wheel turns become the bind.
  function onWheel(e) {
    if (!armed || !row.wheel) return;
    e.preventDefault();
    const mods = eventMods(e);
    const what = [modsName(mods), 'Wheel'].filter(Boolean).join('+');
    const verdict = checkWheelBind(mods);
    if (verdict !== 'ok') { refuse(verdict, what); return; }
    onChange({ [row.modsKey]: String(mods) });
    armed = false; preCapture = null; refusal = '';
    liveMsg = 'Bound to ' + what;
  }
  function onContextMenu() {
    if (skipContextMenu) { skipContextMenu = false; return; }
    clear();
  }
  // Right-click clears the binding (Unbound). Works whether or not the keycap is armed.
  function clear() {
    const patch = {};
    for (const k of [row.vkKey, row.buttonKey, row.modsKey, row.buttonModsKey]) if (k) patch[k] = '0';
    onChange(patch);
    armed = false; preCapture = null; refusal = '';
    liveMsg = 'Binding cleared. Unbound.';
  }
</script>
<svelte:window on:keydown={onKey} on:mousedown={onMouse} on:wheel|nonpassive={onWheel} />
<!-- The instructions were `title`-only, which a screen reader never reads on keyboard focus.
     They are a real description now, appended to the row's own. -->
<button class="keycap" type="button" class:armed class:split={split && !armed && lbl !== null} class:unbound={!armed && lbl === null} {disabled} id={valueId}
        aria-labelledby={labelledby} aria-describedby="{describedby ?? ''} {uid}-hint"
        on:click={arm}
        on:blur={() => { if (armed) cancel(); }}
        on:contextmenu|preventDefault={onContextMenu}
        title="Click to bind (combos like Ctrl+Alt+F1 work), right-click to clear">
  {#if armed}
    {row.wheel ? 'Hold keys and turn the wheel...' : row.buttonKey ? 'Press a key, combo, or button...' : 'Press a key or combo...'}
  {:else if split && lbl !== null}
    {#each caps as c, i}{#if i}<span class="pl">+</span>{/if}<span class="kc">{c}</span>{/each}
  {:else}
    {#if split}<svg class="plus" aria-hidden="true" focusable="false" viewBox="0 0 16 16"><path d="M8 3v10M3 8h10"/></svg>{/if}{lbl ?? unboundText}
  {/if}
</button>
{#if armed && refusal}<span class="refusal">{refusal}</span>{/if}
<span class="sr-only" id="{uid}-hint" aria-hidden="true">
  Activate to rebind{row.buttonKey ? ', then press a key, a combination, or a mouse side-button' : ', then press a key or a combination'}. Escape cancels. Right-click, or use the context-menu key, to clear the binding.
</span>
<span class="sr-only" role="status" aria-live="assertive">{liveMsg}</span>
<style>
  /* Ported from mockups/config-ui-onboarding.html .keycap. */
  .keycap { padding: 4px 10px; border-radius: 6px; border: 1px solid var(--line); background: var(--chip); font-size: 11.5px; color: var(--text); cursor: pointer; }
  .keycap.armed { outline: 2px solid var(--accent); }
  .keycap:disabled { opacity: .5; cursor: default; }
  .refusal { display: block; margin-top: 4px; font-size: 11.5px; color: var(--warn, #e0a030); max-width: 280px; }
</style>

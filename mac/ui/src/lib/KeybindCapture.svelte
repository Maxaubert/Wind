<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // One keycap that records a bind (the onboarding's zoom keys; the Settings page uses
  // controls/Bindings.svelte). Wind for Mac: row.key names the config list (values[row.key] is a comma
  // list of Mac bind texts, '' = unbound); a captured press REPLACES the list with that one bind
  // ("ctrl+opt+equal", "mouse4"). row.wheelMods records modifiers only ("ctrl"; '' = off).
  // `onChange(patch)` is the live setter (setConfig per key, and the parent's values). Every capture
  // goes through the shared rules (keybindRules.js, the mirror of Binds.swift bindVerdict); a refused
  // press keeps the keycap listening and says why, on screen and to screen readers.
  // While armed, Wind's own hotkeys are paused (captureBinds), so the old bind still reaches the page.
  import { refusalText, parseBind } from './keybindRules.js';
  import { eventMods, keyEventBind, mouseEventBind, formatBind, bindLabel, bindSpoken, bindVerdict, splitBinds } from './bindings.js';
  import { bindParts, modsGlyphs, modsSpoken } from './bindNames.js';
  import { setCapturing, capturing } from './capture.js';
  import { captureBinds } from '../bridge.js';
  import { onDestroy } from 'svelte';
  // `onClear` (optional) replaces the right-click clear patch.
  export let row, values, onChange, disabled = false, onClear = null;
  // What an empty keycap says.
  export let unboundText = 'Unbound';
  // Show a bind as its modifier glyphs and key in separate caps (⌃⌥ + =); off: one label (⌃⌥=).
  export let split = false;
  // Naming from the owning row: `labelledby` lists the row label AND the value span.
  export let labelledby = undefined, describedby = undefined, valueId = undefined;
  let refusal = '';
  // A click captured ON the keycap is followed by its own click event, which would re-arm it.
  let mouseCapturedAt = -Infinity;
  // A right-click WITH modifiers is a bind attempt; the contextmenu that follows must not clear.
  let skipContextMenu = false;
  let armed = false;
  let peak = [];   // wheelMods: the most modifiers held at once since arming
  let liveMsg = '';
  const uid = 'kc-' + Math.random().toString(36).slice(2, 8);

  $: setCapture(armed);
  function setCapture(on) { setCapturing(uid, on); captureBinds(capturing()); }
  onDestroy(() => { if (armed) { armed = false; } setCapturing(uid, false); captureBinds(capturing()); });

  // The binds as display entries: [{ label, spoken, caps }].
  $: shown = (function () {
    const v = String((values && values[row.key]) ?? '');
    if (row.wheelMods) {
      const b = v ? parseBind(v) : null;
      if (!b) return v ? [{ label: v, spoken: v, caps: [v] }] : [];
      return [{ label: modsGlyphs(b.mods) + ' Scroll', spoken: modsSpoken(b.mods) + ' and scroll', caps: [modsGlyphs(b.mods), 'Scroll'] }];
    }
    return splitBinds(v).map((t) => {
      const b = parseBind(t);
      if (!b) return { label: t, spoken: t, caps: [t] };
      const p = bindParts(b);
      return { label: bindLabel(b), spoken: bindSpoken(b), caps: [p.mods, p.main].filter(Boolean) };
    });
  })();
  $: lbl = shown.length ? shown.map((s) => s.label).join(' or ') : null;

  function refuse(verdict, what) { refusal = refusalText(verdict, what); liveMsg = refusal; }
  function arm() {
    if (disabled || armed) return;
    if (performance.now() - mouseCapturedAt < 500) return;
    refusal = ''; peak = [];
    liveMsg = row.wheelMods
      ? 'Listening. Hold the modifier keys you want, for example Control, then let go, or scroll while holding them. Escape cancels, Tab leaves.'
      : 'Listening. Press a key with modifiers, an F key, or a mouse button. Escape cancels, Tab leaves.';
    armed = true;
  }
  function cancel() {
    armed = false; refusal = ''; peak = [];
    liveMsg = 'Cancelled. Binding unchanged.';
  }
  function done(text, spoken) {
    onChange({ [row.key]: text });
    armed = false; refusal = ''; peak = [];
    liveMsg = 'Bound to ' + spoken;
  }
  function tryBind(b) {
    const v = bindVerdict(b);
    if (v !== 'ok') { refuse(v, bindLabel(b)); return; }
    done(formatBind(b), bindSpoken(b));
  }
  function commitMods(m) {
    if (!m.length) { refuse('empty', ''); return; }
    done(formatBind({ mods: m }), modsSpoken(m) + ' and scroll');
  }
  function onKey(e) {
    if (!armed) return;
    const m = eventMods(e);
    if (e.key === 'Escape' && !m.length) { e.preventDefault(); cancel(); return; }
    // Tab stays a navigation key; the blur handler below cancels the capture.
    if (e.key === 'Tab' && !m.length) return;
    e.preventDefault();
    if (row.wheelMods) { for (const x of m) if (!peak.includes(x)) peak = [...peak, x]; return; }
    const k = keyEventBind(e);
    if (!k || k.kind === 'mod') return;
    if (k.kind === 'unknown') { refuse('invalid', 'That key'); return; }
    tryBind(k.bind);
  }
  function onKeyUp(e) {
    if (!armed || !row.wheelMods || !peak.length || eventMods(e).length) return;
    commitMods(peak);
  }
  function onMouse(e) {
    if (!armed || row.wheelMods) return;
    const b = mouseEventBind(e);
    if (!b) return;
    if ((b.mouse === 1 && !b.mods.length) || (e.button === 0 && e.ctrlKey && b.mods.length === 1)) return;   // right-click clears (contextmenu)
    const v = bindVerdict(b);
    // A refused press on the armed keycap stays armed and says why. Anywhere else (Next, the other
    // row) it is the user leaving: cancel and let it through.
    if (v !== 'ok' && !(e.target instanceof Element && e.target.closest('button') === document.activeElement)) { cancel(); return; }
    e.preventDefault();
    if (b.mouse === 1) skipContextMenu = true;
    if (v !== 'ok') { refuse(v, bindLabel(b)); return; }
    mouseCapturedAt = performance.now();
    tryBind(b);
  }
  function onWheel(e) {
    if (!armed || !row.wheelMods || !e.deltaY) return;
    e.preventDefault();
    commitMods(eventMods(e));
  }
  function onContextMenu() {
    if (skipContextMenu) { skipContextMenu = false; return; }
    clear();
  }
  // Right-click clears the binding (Unbound). Works whether or not the keycap is armed.
  function clear() {
    if (onClear) onClear(); else onChange({ [row.key]: '' });
    armed = false; refusal = ''; peak = [];
    liveMsg = 'Binding cleared. Unbound.';
  }
</script>
<svelte:window on:keydown={onKey} on:keyup={onKeyUp} on:mousedown={onMouse} on:wheel|nonpassive={onWheel} />
<button class="keycap" type="button" class:armed class:split={split && !armed && lbl !== null} class:unbound={!armed && lbl === null} {disabled} id={valueId}
        aria-labelledby={labelledby} aria-describedby="{describedby ?? ''} {uid}-hint"
        on:click={arm}
        on:blur={() => { if (armed) cancel(); }}
        on:contextmenu|preventDefault={onContextMenu}
        title="Click to set a key and right click to clear">
  {#if armed}
    {row.wheelMods ? 'Hold the modifier keys...' : 'Press a key, combo, or button...'}
  {:else if split && lbl !== null}
    {#each shown as s, i}{#if i}<span class="pl">or</span>{/if}{#each s.caps as c, j}{#if j}<span class="pl">+</span>{/if}<span class="kc">{c}</span>{/each}{/each}
  {:else}
    {#if split}<svg class="plus" aria-hidden="true" focusable="false" viewBox="0 0 16 16"><path d="M8 3v10M3 8h10"/></svg>{/if}{lbl ?? unboundText}
  {/if}
</button>
{#if armed && refusal}<span class="refusal">{refusal}</span>{/if}
<span class="sr-only" id="{uid}-hint" aria-hidden="true">
  Activate to rebind{row.wheelMods ? ', then press the modifier keys' : ', then press a key with modifiers, an F key, or a mouse button'}. Escape cancels. Right-click, or use the context-menu key, to clear the binding.
</span>
<span class="sr-only" role="status" aria-live="assertive">{liveMsg}</span>
<style>
  /* Ported from mockups/config-ui-onboarding.html .keycap. */
  .keycap { padding: 4px 10px; border-radius: var(--srad); border: 1px solid var(--line); background: var(--chip); font-size: 11.5px; color: var(--text); cursor: pointer; }
  .keycap.armed { border-color: var(--accent); }
  .keycap:disabled { opacity: .5; cursor: default; }
  .refusal { display: block; margin-top: 4px; font-size: 11.5px; color: var(--warn, #e0a030); max-width: 280px; }
  .pl { margin: 0 4px; opacity: .6; }
</style>

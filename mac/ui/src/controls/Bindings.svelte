<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // The binds of one hotkey row (Wind for Mac). values[row.key] is a comma list of Mac bind texts
  // ("mouse4,ctrl+opt+equal"); the page shows one box per bind ([⌃⌥=]), click a box to record it again,
  // a small x on hover removes it, + adds one up to `row.max`. A bind is any modifiers plus one key or
  // mouse button (Binds.swift); the rules in lib/keybindRules.js (the mirror of bindProblem) refuse
  // the ones that would break typing or app shortcuts, and a refused press stays armed and says why.
  //
  // row.wheelMods: the "zoom with the scroll wheel" row. Its value is modifiers only ("ctrl",
  // "ctrl+opt", '' = off): hold them and let go, or scroll while holding them.
  //
  // Captures write through live(patch) at once. While a box records, Wind's own hotkeys are paused
  // (captureBinds), so the old bind reaches the page and stays in the config until the new one lands.
  import { refusalText, parseBind } from '../lib/keybindRules.js';
  import { unitsOf, removePatch, placePatch, maxOf, eventMods, keyEventBind, mouseEventBind,
           formatBind, bindLabel, bindSpoken, bindVerdict } from '../lib/bindings.js';
  import { modsGlyphs, modsSpoken } from '../lib/bindNames.js';
  import { setCapturing, capturing } from '../lib/capture.js';
  import { captureBinds } from '../bridge.js';
  import { onDestroy } from 'svelte';
  let { row, values = {}, live = () => {}, disabled = false, labelledby, describedby, valueId } = $props();

  const wheel = $derived(!!row.wheelMods);
  // The wheel row is one box of modifiers plus the wheel.
  const wheelUnits = $derived.by(() => {
    const t = String(values[row.key] ?? '');
    const b = t ? parseBind(t) : null;
    if (!t) return [];
    const ok = !!b && b.key === undefined && b.mouse === undefined;
    return [{ idx: 0, text: t, ok, mods: ok ? modsGlyphs(b.mods) : '', main: ok ? 'Scroll' : t,
              label: ok ? modsGlyphs(b.mods) + ' Scroll' : t, spoken: ok ? modsSpoken(b.mods) + ' and scroll' : t }];
  });
  const units = $derived(wheel ? wheelUnits : unitsOf(row, values));
  const max = $derived(wheel ? 1 : maxOf(row));

  // armed: null | { idx } (idx null = adding a new bind).
  let armed = $state(null);
  let refusal = $state('');
  let liveMsg = $state('');
  let peak = [];          // wheel row: the most modifiers held at once since arming
  let capturedAt = -Infinity;
  let skipContext = false;
  const showGhost = $derived(armed ? armed.idx === null : units.length < max);
  const uid = 'bx-' + Math.random().toString(36).slice(2, 8);
  // The page's own shortcuts stand down while any box listens (lib/capture.js), and Wind's hotkeys
  // pause (captureBinds sends the state of all boxes, so the order two boxes change in does not matter).
  $effect(() => {
    setCapturing(uid, !!armed); captureBinds(capturing());
    return () => { setCapturing(uid, false); captureBinds(capturing()); };
  });
  onDestroy(() => cancel(true));

  function arm(idx) {
    if (disabled || armed) return;
    if (performance.now() - capturedAt < 500) return;   // the click that follows a mouse capture
    refusal = ''; peak = [];
    armed = { idx };
    liveMsg = wheel
      ? 'Listening. Hold the modifier keys you want, for example Control, then let go, or scroll while holding them. Escape cancels, Tab leaves.'
      : 'Listening. Press a key with modifiers, an F key, or a mouse button. Escape cancels, Tab leaves.';
  }
  function cancel(quiet = false) {
    if (!armed) return;
    armed = null; refusal = ''; peak = [];
    if (!quiet) liveMsg = 'Cancelled. Binding unchanged.';
  }
  function refuse(verdict, what) { refusal = refusalText(verdict, what); liveMsg = refusal; }
  function commit(text, spoken) {
    live(wheel ? { [row.key]: text } : placePatch(row, values, armed.idx, text));
    armed = null; refusal = ''; peak = [];
    liveMsg = 'Bound to ' + spoken;
  }
  function remove(idx) {
    armed = null; refusal = '';
    live(wheel ? { [row.key]: '' } : removePatch(row, values, idx));
    liveMsg = 'Binding removed.';
  }
  // A full bind (key or mouse button) through the rules.
  function tryBind(b) {
    const v = bindVerdict(b);
    if (v !== 'ok') { refuse(v, bindLabel(b)); return false; }
    commit(formatBind(b), bindSpoken(b));
    return true;
  }
  function commitMods(mods) {
    if (!mods.length) { refuse('empty', ''); return; }
    commit(formatBind({ mods }), modsSpoken(mods) + ' and scroll');
  }

  function onKey(e) {
    if (!armed) return;
    const mods = eventMods(e);
    // Plain Escape cancels and plain Tab stays navigation (the blur cancels); with modifiers they bind.
    if (e.key === 'Escape' && !mods.length) { e.preventDefault(); cancel(); return; }
    if (e.key === 'Tab' && !mods.length) return;
    e.preventDefault();
    const k = keyEventBind(e);
    if (wheel) { for (const m of mods) if (!peak.includes(m)) peak = [...peak, m]; return; }
    if (!k || k.kind === 'mod') return;
    if (k.kind === 'unknown') { refuse('invalid', 'That key'); return; }
    tryBind(k.bind);
  }
  function onKeyUp(e) {
    if (!armed || !wheel || !peak.length || eventMods(e).length) return;
    commitMods(peak);
  }
  function onMouse(e) {
    if (!armed || wheel) return;
    const b = mouseEventBind(e);
    if (!b) return;
    // A plain right-click (or Control-click, the Mac's right-click) removes a bind via contextmenu.
    if ((b.mouse === 1 && !b.mods.length) || (e.button === 0 && e.ctrlKey && b.mods.length === 1)) return;
    const v = bindVerdict(b);
    // A refused press on the box being recorded stays armed and says why. Anywhere else it is the
    // user leaving (another row, the sidebar): cancel and let it through. preventDefault there would
    // also keep the focus, so the blur that cancels would never come.
    if (v !== 'ok' && !(e.target instanceof Element && e.target.closest('button') === document.activeElement)) {
      cancel();
      return;
    }
    e.preventDefault();
    if (b.mouse === 1) skipContext = true;
    if (v !== 'ok') { refuse(v, bindLabel(b)); return; }
    capturedAt = performance.now();
    tryBind(b);
  }
  function onWheel(e) {
    if (!armed || !wheel || !e.deltaY) return;
    e.preventDefault();
    commitMods(eventMods(e));
  }
  // Not passive: a captured scroll must not also scroll the page.
  $effect(() => {
    if (!armed || !wheel) return;
    window.addEventListener('wheel', onWheel, { passive: false });
    return () => window.removeEventListener('wheel', onWheel);
  });
  function onContext(e, idx) {
    e.preventDefault();
    if (skipContext) { skipContext = false; return; }
    remove(idx);
  }
  const what = $derived(wheel ? 'scroll modifiers' : 'key');
</script>

<svelte:window onkeydown={onKey} onkeyup={onKeyUp} onmousedown={onMouse} />

<div class="bx" role="group" aria-labelledby={labelledby && labelledby.split(' ')[0]} aria-describedby={describedby}>
  {#each units as u, i (u.idx + ':' + u.text)}
    {#if i}<span class="pl" aria-hidden="true">or</span>{/if}
    {@const here = !!armed && armed.idx === u.idx}
    <span class="kb">
      <button type="button" class="kchg" {disabled} id={i === 0 ? valueId : undefined}
              aria-label={here ? 'Recording ' + row.label : 'Change ' + row.label + ' binding: ' + u.spoken + (u.ok ? '' : ', not used')}
              aria-describedby="{describedby ?? ''} {uid}-hint"
              title={u.ok ? u.text : 'Wind cannot use "' + u.text + '"'}
              onclick={() => arm(u.idx)} onblur={() => { if (here) cancel(); }}
              oncontextmenu={(e) => onContext(e, u.idx)}>
        {#if here}<span class="kc live">{wheel ? 'Hold modifiers...' : 'Press keys...'}</span>
        {:else}<span class="kc" class:bad={!u.ok}>{#if u.mods}<span class="km">{u.mods}</span>{/if}{#if wheel && u.ok}<span class="kp">+</span>{/if}{u.main}</span>{/if}
      </button>
      {#if !here && !armed}
        <button type="button" class="kx" {disabled} aria-label="Remove binding {u.spoken}" onclick={() => remove(u.idx)}>
          <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/></svg>
        </button>
      {/if}
    </span>
  {/each}
  {#if showGhost}
    {#if units.length}<span class="pl" aria-hidden="true">or</span>{/if}
    <button type="button" class="kc ghost" class:live={!!armed} {disabled} id={units.length ? undefined : valueId}
            aria-label={(units.length ? 'Add ' : 'Set ') + row.label + ' ' + what}
            aria-describedby="{describedby ?? ''} {uid}-hint"
            onclick={() => arm(null)} onblur={() => { if (armed && armed.idx === null) cancel(); }}>
      {#if armed}{wheel ? 'Hold modifiers...' : 'Press keys...'}{:else}<svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M8 3v10M3 8h10"/></svg>{wheel ? 'Set modifiers' : units.length ? 'Add key' : 'Set key'}{/if}
    </button>
  {/if}
  {#if armed && refusal}<span class="refusal">{refusal}</span>{/if}
  <span class="sr-only" id="{uid}-hint">Activate to record a binding. Escape cancels. Right-click, or the remove button, deletes it.</span>
  <span class="sr-only" role="status" aria-live="assertive">{liveMsg}</span>
</div>

<style>
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
  /* The modifier glyphs sit tight against each other and the key, as in macOS menus. */
  .km { letter-spacing: 1px; margin-right: 1px; }
  .kc.bad { text-decoration: line-through; opacity: .7; }
</style>

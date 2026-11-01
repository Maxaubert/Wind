<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // "Pan with the arrow keys" (Wind for Mac): ONE binding of modifiers plus the fixed arrow keys, drawn
  // as a single box [⌃⌥ + Arrow keys] with the arrow part dimmer. Unset it reads [Set modifiers + Arrow
  // keys]. Click to record the modifiers again (hold them and let go, or press an arrow with them held),
  // hover shows an x to remove. It writes the four pan lists (row.panKeys: panLeft, panUp, panRight,
  // panDown) as "<mods>+left" and so on in one live(patch); removing writes '' to all four (pan off).
  // Lists edited by hand into anything else read as "Custom keys" until recorded again.
  import { refusalText } from '../lib/keybindRules.js';
  import { modsGlyphs, modsSpoken } from '../lib/bindNames.js';
  import { panState, panPatch, panBinds, eventMods, bindVerdict } from '../lib/bindings.js';
  import { setCapturing, capturing } from '../lib/capture.js';
  import { captureBinds } from '../bridge.js';
  let { row, values = {}, live = () => {}, disabled = false, labelledby, describedby, valueId } = $props();

  const state = $derived(panState(row, values));
  let armed = $state(false);
  let peak = [];
  let refusal = $state('');
  let liveMsg = $state('');
  const uid = 'pan-' + Math.random().toString(36).slice(2, 8);
  $effect(() => {
    setCapturing(uid, armed); captureBinds(capturing());
    return () => { setCapturing(uid, false); captureBinds(capturing()); };
  });
  const ARROW_CODES = ['ArrowLeft', 'ArrowUp', 'ArrowRight', 'ArrowDown'];

  function arm() {
    if (disabled || armed) return;
    refusal = ''; peak = [];
    armed = true;
    liveMsg = 'Listening. Hold the modifier keys you want, for example Control and Option, then let go. The arrow keys are added automatically. Escape cancels, Tab leaves.';
  }
  function cancel() {
    if (!armed) return;
    armed = false; refusal = ''; peak = [];
    liveMsg = 'Cancelled. Binding unchanged.';
  }
  function remove() {
    armed = false; refusal = ''; peak = [];
    live(panPatch(row, []));
    liveMsg = 'Pan keys removed.';
  }
  function commit(m) {
    for (const b of panBinds(m)) {
      const v = bindVerdict(b);
      if (v !== 'ok') { peak = []; refusal = refusalText(v, modsGlyphs(m) + ' + Arrow keys'); liveMsg = refusal; return; }
    }
    live(panPatch(row, m));
    armed = false; refusal = ''; peak = [];
    liveMsg = 'Bound to ' + modsSpoken(m) + ' plus the arrow keys';
  }
  function onKey(e) {
    if (!armed) return;
    const m = eventMods(e);
    if (e.key === 'Escape' && !m.length) { e.preventDefault(); cancel(); return; }
    if (e.key === 'Tab' && !m.length) return;
    e.preventDefault();
    for (const x of m) if (!peak.includes(x)) peak = [...peak, x];
    if (ARROW_CODES.includes(e.code) && m.length) commit(m);
  }
  function onKeyUp(e) {
    if (!armed || !peak.length || eventMods(e).length) return;
    commit(peak);
  }
  function onContext(e) { e.preventDefault(); remove(); }
</script>

<svelte:window onkeydown={onKey} onkeyup={onKeyUp} />

{#snippet arrows()}
  <span class="kfix"><svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M8 2.5v4M6.5 4 8 2.5 9.5 4M8 13.5v-4M6.5 12 8 13.5 9.5 12M2.5 8h4M4 6.5 2.5 8 4 9.5M13.5 8h-4M12 6.5 13.5 8 12 9.5"/></svg>Arrow keys</span>
{/snippet}

<div class="bx" role="group" aria-labelledby={labelledby && labelledby.split(' ')[0]} aria-describedby={describedby}>
  {#if state.kind === 'none'}
    <button type="button" class="kc ghost pan" class:live={armed} {disabled} id={valueId}
            aria-label={armed ? 'Recording pan modifiers' : 'Set pan modifiers'}
            aria-describedby="{describedby ?? ''} {uid}-hint"
            onclick={arm} onblur={() => cancel()}>
      {armed ? 'Press modifiers...' : 'Set modifiers'}<span class="kp">+</span>{@render arrows()}
    </button>
  {:else}
    <span class="kb">
      <button type="button" class="kchg" {disabled} id={valueId}
              aria-label={armed ? 'Recording pan modifiers' : state.kind === 'custom' ? 'Change pan keys: custom keys' : 'Change pan modifiers: ' + modsSpoken(state.mods)}
              aria-describedby="{describedby ?? ''} {uid}-hint" onclick={arm} onblur={() => cancel()} oncontextmenu={onContext}>
        <span class="kc" class:live={armed}>
          {#if armed}Press modifiers...<span class="kp">+</span>{@render arrows()}
          {:else if state.kind === 'custom'}Custom keys
          {:else}<span class="km">{modsGlyphs(state.mods)}</span><span class="kp">+</span>{@render arrows()}{/if}
        </span>
      </button>
      {#if !armed}
        <button type="button" class="kx" {disabled} aria-label="Remove pan keys" onclick={remove}>
          <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/></svg>
        </button>
      {/if}
    </span>
  {/if}
  {#if armed && refusal}<span class="refusal">{refusal}</span>{/if}
  <span class="sr-only" id="{uid}-hint">Activate to record the modifiers. Escape cancels. Right-click, or the remove button, deletes it.</span>
  <span class="sr-only" role="status" aria-live="assertive">{liveMsg}</span>
</div>

<style>
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
  .km { letter-spacing: 1px; }
</style>

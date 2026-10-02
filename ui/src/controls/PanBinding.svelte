<script>
  // "Pan with the arrow keys" (Hotkeys page, #318): ONE binding of one or two modifiers plus the fixed
  // arrow keys, drawn as a single box [Ctrl + Alt + Arrow keys] with the arrow part dimmer. Unset it
  // reads [Set modifiers + Arrow keys]. Click to record the modifiers again (hold them and let go, or
  // press an arrow with them held), hover shows an x to remove. It reads and writes the four real pan
  // binds (left, up, right, down) as one, through live(patch) like every keybind.
  import { checkKeyBind, refusalText } from '../lib/keybindRules.js';
  import { modList, popcount } from '../lib/bindNames.js';
  import { PAN_ARROWS, panState, panPatch } from '../lib/bindings.js';
  let { row, values = {}, live = () => {}, disabled = false, labelledby, describedby, valueId } = $props();

  const now = $derived(panState(values));
  let armed = $state(false);
  let snap = $state(null);   // the state when arming: arming clears a bound pan, the box must stay put
  const state = $derived(armed && snap ? snap : now);
  let pre = null;
  let peak = 0;
  let refusal = $state('');
  let liveMsg = $state('');
  const uid = 'pan-' + Math.random().toString(36).slice(2, 8);
  const mods = (e) => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);

  function arm() {
    if (disabled || armed) return;
    refusal = ''; peak = 0;
    // Arming clears a bound pan (the core would swallow the keys) but never wipes older custom keys.
    pre = now.kind === 'arrows' ? panPatch(now.mods) : null;
    snap = now;
    if (pre) live(panPatch(0));
    armed = true;
    liveMsg = 'Listening. Hold one or two modifier keys, for example Control and Alt, then let go. The arrow keys are added automatically. Escape cancels, Tab leaves.';
  }
  function cancel() {
    if (!armed) return;
    if (pre) live(pre);
    armed = false; pre = null; refusal = ''; peak = 0;
    liveMsg = 'Cancelled. Binding unchanged.';
  }
  function remove() {
    armed = false; pre = null; refusal = ''; peak = 0;
    live(panPatch(0));
    liveMsg = 'Pan keys removed.';
  }
  function commit(m) {
    if (popcount(m) > 2) { peak = 0; refusal = refusalText('twomods', modList(m).join('+') + '+Arrow keys'); liveMsg = refusal; return; }
    for (const vk of PAN_ARROWS) {
      const v = checkKeyBind(vk, m);
      if (v !== 'ok') { peak = 0; refusal = refusalText(v, modList(m).join('+') + '+Arrow keys'); liveMsg = refusal; return; }
    }
    live(panPatch(m));
    armed = false; pre = null; refusal = ''; peak = 0;
    liveMsg = 'Bound to ' + modList(m).join(' and ') + ' plus the arrow keys';
  }
  function onKey(e) {
    if (!armed) return;
    if (e.key === 'Escape') { e.preventDefault(); cancel(); return; }
    if (e.key === 'Tab') return;
    e.preventDefault();
    peak |= mods(e);
    if (e.keyCode >= 37 && e.keyCode <= 40 && mods(e)) commit(mods(e));
  }
  function onKeyUp(e) {
    if (!armed || !peak || mods(e)) return;
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
    <button type="button" class="kc ghost" class:live={armed} {disabled} id={valueId}
            aria-label={armed ? 'Recording pan modifiers' : 'Set pan modifiers'}
            aria-describedby="{describedby ?? ''} {uid}-hint"
            onclick={arm} onblur={() => cancel()}>
      {armed ? 'Press modifiers...' : 'Set modifiers'}<span class="kp">+</span>{@render arrows()}
    </button>
  {:else}
    <span class="kb">
      <button type="button" class="kchg" {disabled} id={valueId}
              aria-label={armed ? 'Recording pan modifiers' : state.kind === 'custom' ? 'Change pan keys: custom keys' : 'Change pan modifiers: ' + modList(state.mods).join(' + ')}
              aria-describedby="{describedby ?? ''} {uid}-hint" onclick={arm} onblur={() => cancel()} oncontextmenu={onContext}>
        <span class="kc" class:live={armed}>
          {#if armed}Press modifiers...<span class="kp">+</span>{@render arrows()}
          {:else if state.kind === 'custom'}Custom keys
          {:else}{#each modList(state.mods) as c, j}{#if j}<span class="kp">+</span>{/if}{c}{/each}<span class="kp">+</span>{@render arrows()}{/if}
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
</style>

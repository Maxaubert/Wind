<script>
  // The bindings of one hotkey row (Hotkeys page, #318): one box per binding with the plus inside
  // ([Ctrl + Wheel up]), click a box to record it again, a small x on hover removes it. A row takes
  // `row.max` bindings (zoom rows 2, the rest 1); a binding is at most two modifiers plus one key,
  // mouse button or wheel direction. Wheel is recorded on rows that have button slots (the zoom rows).
  //
  // Captures write through `live(patch)` (setConfigPersist) at once. Arming first clears the old
  // binding, because the core swallows a bound key and pressing it again to rebind would never reach
  // this page; Escape, Tab (leaving) and blur put it back.
  import { checkKeyBind, checkClickBind, refusalText } from '../lib/keybindRules.js';
  import { vkName, BUTTON_NAMES, modList, popcount, WHEEL_UP, WHEEL_DOWN } from '../lib/bindNames.js';
  import { unitsOf, clearUnit, placeBinding, maxOf } from '../lib/bindings.js';
  let { row, values = {}, live = () => {}, disabled = false, labelledby, describedby, valueId } = $props();

  const units = $derived(unitsOf(row, values));
  const max = $derived(maxOf(row));
  const wheelOk = $derived(!!row.buttonKey);

  // armed: null | { add: true } | { unit, idx, list }. `list` is the units as they were when arming, so
  // the box being recorded stays on screen while its old binding is cleared in the ini.
  let armed = $state(null);
  let pre = null;
  let refusal = $state('');
  let liveMsg = $state('');
  let capturedAt = -Infinity;
  let skipContext = false;
  const list = $derived(armed && armed.list ? armed.list : units);
  const showGhost = $derived(armed ? !!armed.add : units.length < max);
  const uid = 'bx-' + Math.random().toString(36).slice(2, 8);

  const mods = (e) => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);
  const text = (m, name) => [...modList(m), name].join('+');
  const nameOf = (u) => u.caps.join(' + ');
  const FORBIDDEN_VK = new Set([1, 2, 8, 91, 92]);

  function arm(unit, idx) {
    if (disabled || armed) return;
    if (performance.now() - capturedAt < 500) return;   // the click that follows a mouse capture
    refusal = '';
    if (unit) {
      const patch = clearUnit(row, unit);
      pre = Object.fromEntries(Object.keys(patch).map((k) => [k, String(values[k] ?? '0')]));
      armed = { unit, idx, list: units };
      live(patch);
    } else { pre = null; armed = { add: true }; }
    liveMsg = 'Listening. Press a key or a combination' + (row.buttonKey ? ', a mouse button, or turn the mouse wheel while holding a modifier' : '')
      + '. At most two modifiers. Escape cancels, Tab leaves.';
  }
  function cancel() {
    if (!armed) return;
    if (pre) live(pre);
    armed = null; pre = null; refusal = '';
    liveMsg = 'Cancelled. Binding unchanged.';
  }
  function refuse(verdict, what) { refusal = refusalText(verdict, what); liveMsg = refusal; }
  function commit(captured, name) {
    live(placeBinding(row, values, armed.unit || null, captured));
    armed = null; pre = null; refusal = '';
    liveMsg = 'Bound to ' + name;
  }
  function remove(unit) {
    armed = null; pre = null; refusal = '';
    live(clearUnit(row, unit));
    liveMsg = 'Binding removed.';
  }

  function onKey(e) {
    if (!armed) return;
    if (e.key === 'Escape') { e.preventDefault(); cancel(); return; }
    if (e.key === 'Tab') return;   // stays navigation; the blur below cancels
    e.preventDefault();
    if (!e.keyCode || [16, 17, 18, 91, 92].includes(e.keyCode)) return;
    const m = mods(e);
    if (FORBIDDEN_VK.has(e.keyCode)) { refuse('never', vkName(e.keyCode)); return; }
    if (popcount(m) > 2) { refuse('twomods', text(m, vkName(e.keyCode))); return; }
    const v = checkKeyBind(e.keyCode, row.modsKey ? m : 0);
    if (v !== 'ok') { refuse(v, row.modsKey ? text(m, vkName(e.keyCode)) : vkName(e.keyCode)); return; }
    commit({ kind: 'key', vk: e.keyCode, mods: row.modsKey ? m : 0 }, text(row.modsKey ? m : 0, vkName(e.keyCode)));
  }
  function onMouse(e) {
    if (!armed || !row.buttonKey) return;
    // DOM buttons: 0 left, 1 middle, 2 right, 3 back, 4 forward -> slot ids 3, 5, 4, 1, 2.
    const btn = { 0: 3, 1: 5, 2: 4, 3: 1, 4: 2 }[e.button];
    if (!btn) return;
    const m = mods(e);
    if (btn === 4 && m === 0) return;   // a plain right-click removes the binding (contextmenu)
    e.preventDefault();
    if (btn === 4) skipContext = true;
    const name = text(m, BUTTON_NAMES[btn]);
    if (popcount(m) > 2) { refuse('twomods', name); return; }
    const v = checkClickBind(btn, m);
    if (v !== 'ok') { refuse(v, name); return; }
    capturedAt = performance.now();
    commit({ kind: 'btn', code: btn, mods: m }, name);
  }
  function onWheel(e) {
    if (!armed || !wheelOk || !e.deltaY) return;
    e.preventDefault();
    const code = e.deltaY < 0 ? WHEEL_UP : WHEEL_DOWN;
    const m = mods(e);
    const name = text(m, BUTTON_NAMES[code]);
    if (popcount(m) > 2) { refuse('twomods', name); return; }
    const v = checkClickBind(code, m);
    if (v !== 'ok') { refuse(v, name); return; }
    commit({ kind: 'btn', code, mods: m }, name);
  }
  // Not passive: a captured notch must not also scroll the page.
  $effect(() => {
    if (!armed) return;
    window.addEventListener('wheel', onWheel, { passive: false });
    return () => window.removeEventListener('wheel', onWheel);
  });
  function onContext(e, unit) {
    e.preventDefault();
    if (skipContext) { skipContext = false; return; }
    remove(unit);
  }
</script>

<svelte:window onkeydown={onKey} onmousedown={onMouse} />

<div class="bx" role="group" aria-labelledby={labelledby && labelledby.split(' ')[0]} aria-describedby={describedby}>
  {#each list as u, i (u.slot + u.part)}
    {#if i}<span class="pl" aria-hidden="true">or</span>{/if}
    {@const here = !!armed && armed.idx === i && !!armed.unit}
    <span class="kb">
      <button type="button" class="kchg" {disabled} id={i === 0 ? valueId : undefined}
              aria-label={here ? 'Recording ' + row.label : 'Change ' + row.label + ' binding: ' + nameOf(u)}
              aria-describedby="{describedby ?? ''} {uid}-hint"
              onclick={() => arm(u, i)} onblur={() => { if (here) cancel(); }}
              oncontextmenu={(e) => onContext(e, u)}>
        {#if here}<span class="kc live">Press keys...</span>
        {:else}<span class="kc">{#each u.caps as c, j}{#if j}<span class="kp">+</span>{/if}{c}{/each}</span>{/if}
      </button>
      {#if !here && !armed}
        <button type="button" class="kx" {disabled} aria-label="Remove binding {nameOf(u)}" onclick={() => remove(u)}>
          <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/></svg>
        </button>
      {/if}
    </span>
  {/each}
  {#if showGhost}
    {#if list.length}<span class="pl" aria-hidden="true">or</span>{/if}
    <button type="button" class="kc ghost" class:live={!!armed} {disabled} id={list.length ? undefined : valueId}
            aria-label={(list.length ? 'Add ' : 'Set ') + row.label + ' key'}
            aria-describedby="{describedby ?? ''} {uid}-hint"
            onclick={() => arm(null)} onblur={() => { if (armed && armed.add) cancel(); }}>
      {#if armed}Press keys...{:else}<svg viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="M8 3v10M3 8h10"/></svg>{list.length ? 'Add key' : 'Set key'}{/if}
    </button>
  {/if}
  {#if armed && refusal}<span class="refusal">{refusal}</span>{/if}
  <span class="sr-only" id="{uid}-hint">Activate to record a binding. Escape cancels. Right-click, or the remove button, deletes it.</span>
  <span class="sr-only" role="status" aria-live="assertive">{liveMsg}</span>
</div>

<style>
  .sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden;
             clip: rect(0 0 0 0); white-space: nowrap; border: 0; }
</style>

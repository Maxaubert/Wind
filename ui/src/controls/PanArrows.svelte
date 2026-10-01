<script>
  // The "Pan with the arrow keys" control: [modifier slot] + four fixed arrow keycaps. Panning always
  // uses the arrow keys, so the caps are drawn bound or not, dimmed and inert; only the modifier slot
  // is interactive. It reads and writes the four real pan binds (left, up, right, down) as one.
  import KeybindCapture from '../lib/KeybindCapture.svelte';
  let { row, values = {}, live = () => {}, disabled = false, labelledby, describedby, valueId } = $props();

  const ARROWS = [{ vk: 37, g: '←', n: 'Left' }, { vk: 38, g: '↑', n: 'Up' },
                  { vk: 39, g: '→', n: 'Right' }, { vk: 40, g: '↓', n: 'Down' }];
  const VK = ['panLeftVk', 'panUpVk', 'panRightVk', 'panDownVk'];
  const MODS = ['panLeftMods', 'panUpMods', 'panRightMods', 'panDownMods'];
  const n = (k) => Number(values[k] || 0);

  // unbound: all four keys 0. layout: the four arrows with one shared modifier mask. Anything else
  // (custom keys from an older version) is shown as "Custom keys" until the user picks modifiers.
  const state = $derived.by(() => {
    if (VK.every((k) => n(k) === 0)) return { kind: 'none', mods: 0 };
    const mods = n(MODS[0]);
    const arrows = VK.every((k, i) => n(k) === ARROWS[i].vk) && MODS.every((k) => n(k) === mods);
    return arrows && mods ? { kind: 'arrows', mods } : { kind: 'custom', mods: 0 };
  });
  const cap = $derived({ modsOnly: true, modsKey: '__panMods' });
  const vals = $derived({ __panMods: String(state.mods) });

  function write(mods) {
    const patch = {};
    VK.forEach((k, i) => { patch[k] = mods ? String(ARROWS[i].vk) : '0'; });
    MODS.forEach((k) => { patch[k] = String(mods); });
    live(patch);
  }
  // Arming and cancelling write a '0' to clear / restore; with custom keys on file that must not wipe
  // them (only a real choice or Reset does).
  function onChange(patch) {
    const m = Number(patch.__panMods || 0);
    if (!m && state.kind === 'custom') return;
    write(m);
  }
</script>

<div class="pan" role="group" aria-labelledby={labelledby && labelledby.split(" ")[0]} aria-describedby={describedby}>
  {#if state.kind === 'custom'}
    <span class="kc custom" id={valueId}>Custom keys</span>
    <KeybindCapture row={cap} values={vals} {onChange} onClear={() => write(0)} {disabled} unboundText="Change" split {describedby} />
    <button type="button" class="reset" {disabled} onclick={() => write(0)}>Reset</button>
  {:else}
    <KeybindCapture row={cap} values={vals} {onChange} onClear={() => write(0)} {disabled} unboundText="Add modifier" split {labelledby} {describedby} {valueId} />
    <span class="pl">+</span>
    <span class="arrows" role="img" aria-label="Left, Up, Right and Down arrow keys">
      {#each ARROWS as a}<span class="kc fixed" aria-hidden="true">{a.g}</span>{/each}
    </span>
  {/if}
</div>

<style>
  .pan { display: inline-flex; align-items: center; gap: 6px; flex-wrap: wrap; justify-content: flex-end; }
  .arrows { display: inline-flex; gap: 4px; }
  /* Fixed keys: the same cap shape, smaller and recessed, with no hover, no focus and a plain cursor. */
  .pan span.kc.fixed { min-width: 26px; height: 24px; padding: 0; font: 13px var(--m); background: transparent;
    border-style: dashed; color: var(--fg3); cursor: default; user-select: none; pointer-events: none; }
  .pan span.kc.custom { color: var(--fg3); }
  .reset { height: 28px; padding: 0 10px; border-radius: 8px; border: 1px solid var(--chipb); background: none;
    color: var(--fg2); font: 12.5px var(--s); }
  .reset:hover:not(:disabled) { border-color: var(--outline); color: var(--fg); }
</style>

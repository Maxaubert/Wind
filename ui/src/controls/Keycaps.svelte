<script>
  // Capture slots for one keybind row. The capture logic (safety rules, live write, Escape restore,
  // screen-reader messages) stays in lib/KeybindCapture.svelte; this restyles it as monospace
  // keycaps with an "or" between the two slots and a soft "Add key" chip when a slot is empty.
  import KeybindCapture from '../lib/KeybindCapture.svelte';
  let { row, values = {}, live = () => {}, disabled = false, labelledby, describedby, valueId } = $props();
  const second = $derived(!!(row.vkKey2 || row.buttonKey2));
  const row2 = $derived({ ...row, buttonKey: row.buttonKey2, vkKey: row.vkKey2, modsKey: row.modsKey2,
                          buttonModsKey: row.buttonModsKey2 });
</script>

<div class="kcs">
  <KeybindCapture {row} {values} onChange={live} {disabled} unboundText="Add key" split
                  {labelledby} {describedby} {valueId} />
  {#if second}
    <span class="sep" aria-hidden="true">or</span>
    <KeybindCapture row={row2} {values} onChange={live} {disabled} unboundText="Add key" split
                    labelledby={[labelledby && labelledby.split(' ')[0], valueId && valueId + '2'].filter(Boolean).join(' ')}
                    {describedby} valueId={valueId && valueId + '2'} />
  {/if}
</div>

<style>
  .kcs { display: inline-flex; align-items: center; gap: 6px; flex-wrap: wrap; justify-content: flex-end; }
  .sep { font: 12px var(--s); color: var(--glyph); min-width: 14px; text-align: center; }
  /* Keycap: 28px, radius 8, regular 12px mono in fg2 (FINAL-v10-grey.html .kc). A combo is one cap
     per key with a '+' between; the button itself is then only the clickable wrapper. */
  .kcs :global(.kc), .kcs :global(button.keycap) { height: 28px; min-width: 44px; padding: 0 10px; display: inline-flex;
    align-items: center; justify-content: center; border: 1px solid var(--chipb); background: var(--chip);
    border-radius: 8px; font: 12px var(--m); color: var(--fg2); }
  .kcs :global(button.keycap.split) { min-width: 0; padding: 0; gap: 6px; border: 0; background: none; border-radius: 0; }
  .kcs :global(.pl) { font: 12px var(--s); color: var(--glyph); min-width: 14px; text-align: center; }
  .kcs :global(button.keycap:hover) { border-color: var(--outline); }
  .kcs :global(button.keycap.split:hover) { border-color: transparent; }
  .kcs :global(button.keycap.split:hover .kc) { border-color: var(--outline); }
  /* Empty slot: a filled ghost cap with a leading plus. */
  .kcs :global(button.keycap.unbound) { gap: 5px; border-color: transparent; color: var(--fg3); }
  .kcs :global(button.keycap.unbound:hover) { color: var(--fg); border-color: var(--chipb); }
  .kcs :global(svg.plus) { width: 11px; height: 11px; flex: none; fill: none; stroke: currentColor; stroke-width: 1.75;
    stroke-linecap: round; stroke-linejoin: round; }
  .kcs :global(button.keycap.armed) { outline: 2px solid var(--fg); outline-offset: 1px; }
  .kcs :global(.refusal) { color: var(--fg2); }
</style>

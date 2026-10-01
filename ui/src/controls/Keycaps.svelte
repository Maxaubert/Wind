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
  <KeybindCapture {row} {values} onChange={live} {disabled} unboundText="Add key"
                  {labelledby} {describedby} {valueId} />
  {#if second}
    <span class="sep" aria-hidden="true">or</span>
    <KeybindCapture row={row2} {values} onChange={live} {disabled} unboundText="Add key"
                    labelledby={[labelledby && labelledby.split(' ')[0], valueId && valueId + '2'].filter(Boolean).join(' ')}
                    {describedby} valueId={valueId && valueId + '2'} />
  {/if}
</div>

<style>
  .kcs { display: inline-flex; align-items: center; gap: 8px; flex-wrap: wrap; justify-content: flex-end; }
  .sep { color: var(--fg3); font: 12px var(--m); }
  .kcs :global(.keycap) { font: 500 12px var(--m); color: var(--fg); background: var(--chip);
    border: 1px solid var(--chipb); border-radius: var(--srad); padding: 5px 10px; min-height: 28px; }
  .kcs :global(.keycap:hover) { border-color: var(--outline); }
  .kcs :global(.keycap.unbound) { font-family: var(--s); font-weight: 400; color: var(--fg2); border-color: transparent; }
  .kcs :global(.keycap.armed) { outline: 2px solid var(--fg); outline-offset: 1px; }
  .kcs :global(.refusal) { color: var(--fg2); }
</style>

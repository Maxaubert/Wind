<script>
  // The Engine select. The engine is read once at launch, so a change needs a relaunch:
  // once the chosen value differs from the engine the running Wind has, an inline "Restart Wind"
  // appears beside the select. `running` is that engine (empty = unknown, no button).
  import Select from './Select.svelte';
  let { value = 'hybrid', options = [], labels = null, running = '', onChange = () => {},
        onRestart = () => {}, disabled = false, labelledby, describedby } = $props();
  const pending = $derived(!!running && value !== running);
</script>

<div class="en">
  {#if pending}<button type="button" class="restart" {disabled} onclick={onRestart}>Restart Wind</button>{/if}
  <Select {value} {options} {labels} {onChange} {disabled} {labelledby} {describedby} />
</div>

<style>
  .en { display: inline-flex; align-items: center; gap: 10px; }
  .restart { height: 28px; padding: 0 12px; border-radius: var(--srad); background: var(--accent);
             color: var(--onaccent); font: 600 12.5px var(--s); }
  .restart:hover { opacity: .88; }
</style>

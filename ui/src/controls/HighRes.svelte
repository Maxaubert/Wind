<script>
  // High resolution cursor: the toggle drives txSamplingMode. It applies live (#369), so no restart tag.
  // Turning it on may be held back by the page (the experimental notice, #441), in which case `value`
  // does not change and the switch must not stay drawn as on: reset it to the prop after each flip.
  import { tick } from 'svelte';
  import Toggle from './Toggle.svelte';
  let { value = 0, onChange = () => {}, disabled = false, labelledby, describedby } = $props();
  let root = $state();
  function flip(v) {
    onChange(v);
    tick().then(() => { const i = root && root.querySelector('input'); if (i) i.checked = Number(value) === 1; });
  }
</script>

<div class="hr" bind:this={root}>
  <Toggle {value} onChange={flip} {disabled} {labelledby} {describedby} />
</div>

<style>
  .hr { display: inline-flex; align-items: center; gap: 10px; }
</style>

<script>
  // High resolution cursor: the toggle drives txSamplingMode. It applies live (#369), so no restart tag.
  import Toggle from './Toggle.svelte';
  let { value = 0, onChange = () => {}, disabled = false, labelledby, describedby } = $props();
</script>

<div class="hr">
  <Toggle {value} {onChange} {disabled} {labelledby} {describedby} />
</div>

<style>
  .hr { display: inline-flex; align-items: center; gap: 10px; }
</style>

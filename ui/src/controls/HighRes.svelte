<script>
  // High resolution cursor: the toggle drives txSamplingMode; the page mirrors the staged MPO half
  // (#242). "Requires restart" says an applied change has not taken effect yet.
  import Toggle from './Toggle.svelte';
  let { value = 0, onChange = () => {}, needsRestart = false, disabled = false, labelledby, describedby, tagId } = $props();
</script>

<div class="hr">
  {#if needsRestart}<span class="tag" id={tagId}>Requires restart</span>{/if}
  <Toggle {value} {onChange} {disabled} {labelledby}
          describedby={needsRestart && tagId ? `${describedby ?? ''} ${tagId}`.trim() : describedby} />
</div>

<style>
  .hr { display: inline-flex; align-items: center; gap: 10px; }
  .tag { background: var(--chip); color: var(--fg3); border: 1px solid var(--chipb); border-radius: 999px;
         padding: 2px 9px; font-size: 11px; white-space: nowrap; }
</style>

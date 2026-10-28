<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // The Engine select. A running Wind with both engines up applies a change live on its reload
  // (`running` is "live": no button). One whose other engine failed to start runs that one engine
  // until it restarts: once the chosen value differs from it, an inline "Restart Wind" appears
  // beside the select. `running` empty = unknown, no button.
  import Select from './Select.svelte';
  let { value = 'transform', options = [], labels = null, running = '', onChange = () => {},
        onRestart = () => {}, disabled = false, labelledby, describedby } = $props();
  const pending = $derived(!!running && running !== 'live' && value !== running);
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

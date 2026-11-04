<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // A macOS status row's control (schema type 'macstatus'): what macOS reports now, and the fix.
  //   row.status   accessibility | screenRecording (true = allowed), zoomShortcuts (the list of macOS
  //                Zoom shortcuts that are on; empty = all off) or loginItem (with row.toggle: a switch)
  //   mac          the latest macStatus from the host (bridge.js getMacStatus / onMacStatus), null until known
  //   onAction     onAction(action, value) runs row.action through macAction
  import Toggle from './Toggle.svelte';
  let { row, mac = null, onAction = () => {}, disabled = false, labelledby, describedby, valueId } = $props();
  const known = $derived(!!mac && mac[row.status] !== undefined);
  const ok = $derived(row.status === 'zoomShortcuts' ? !(mac && mac.zoomShortcuts && mac.zoomShortcuts.length) : !!(mac && mac[row.status]));
  const detail = $derived(row.status === 'zoomShortcuts' && mac && mac.zoomShortcuts && mac.zoomShortcuts.length
    ? mac.zoomShortcuts.join(', ') : undefined);
</script>

{#if row.toggle}
  <Toggle value={ok ? 1 : 0} disabled={disabled || !known} onChange={(v) => onAction(row.action, v)}
          {labelledby} {describedby} />
{:else}
  <span class="st" class:ok class:bad={known && !ok} id={valueId} title={detail}>
    <i aria-hidden="true"></i>{known ? (ok ? row.on : row.off) : 'Checking'}
  </span>
  {#if known && !ok}
    <!-- Named by what it does ("Turn off macOS Zoom shortcuts"); the status and the row text describe it. -->
    <button type="button" class="chip" {disabled} aria-label={row.btn + (row.btn === 'Turn off' ? ' ' + row.label : ' for ' + row.label)}
            aria-describedby={valueId + (describedby ? ' ' + describedby : '')}
            onclick={() => onAction(row.action, '')}>{row.btn}</button>
  {/if}
{/if}

<style>
  .st { display: inline-flex; align-items: center; gap: 7px; font: 13px var(--s); color: var(--fg3); margin-right: 6px; }
  .st i { width: 7px; height: 7px; border-radius: 50%; background: var(--track); }
  .st.ok { color: var(--fg2); }
  .st.ok i { background: var(--fill); }
  .st.bad { color: var(--danger); }
  .st.bad i { background: var(--danger); }
  .chip { height: 28px; padding: 0 12px; border-radius: var(--srad); background: var(--chip);
          border: 1px solid var(--chipb); color: var(--fg); font: 13px var(--s); }
  .chip:hover:not(:disabled) { border-color: var(--outline); }
</style>

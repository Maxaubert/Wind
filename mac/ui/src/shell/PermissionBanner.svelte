<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // The warning strip at the top of every page while a macOS permission Wind needs is missing
  // (settings-schema.js missingPermissions): Accessibility always, Screen Recording on the render
  // engine. The button opens the first missing pane in System Settings.
  //   missing   ['accessibility', 'screenRecording'] (any subset; empty renders nothing)
  //   onFix     onFix(action) with openAccessibility or openScreenRecording
  let { missing = [], onFix = () => {} } = $props();
  const NAMES = { accessibility: 'Accessibility', screenRecording: 'Screen Recording' };
  const ACTIONS = { accessibility: 'openAccessibility', screenRecording: 'openScreenRecording' };
  const title = $derived(missing.length > 1 ? 'Wind needs two permissions'
    : missing[0] === 'accessibility' ? 'Wind needs Accessibility' : 'Wind needs Screen Recording');
  const text = $derived(missing.includes('accessibility')
    ? 'Without it Wind cannot read your zoom keys. Allow Wind in ' + missing.map((m) => NAMES[m]).join(' and ') + ', then come back here.'
    : 'Without it the render engine cannot draw the zoomed screen. Allow Wind in Screen Recording, then come back here.');
</script>

{#if missing.length}
  <div class="pb" role="alert" data-missing={missing.join(' ')}>
    <svg class="ic" viewBox="0 0 16 16" width="18" height="18" aria-hidden="true" focusable="false">
      <path d="M8 1.75 14.75 13.75H1.25z"/><path d="M8 6v4M8 11.5v1"/>
    </svg>
    <div class="t">
      <div class="h">{title}</div>
      <div class="d">{text}</div>
    </div>
    <button type="button" class="fix" onclick={() => onFix(ACTIONS[missing[0]])}>Open System Settings</button>
  </div>
{/if}

<style>
  .pb { margin: 18px 0 0; display: flex; align-items: center; gap: 14px; padding: 12px 16px;
        border: 1px solid color-mix(in srgb, var(--danger) 40%, transparent); background: var(--dangerbg);
        border-radius: var(--rc); color: var(--fg); }
  .ic { color: var(--danger); flex: none; }
  .t { flex: 1; min-width: 0; }
  .h { font: 600 13.5px var(--s); }
  .d { margin-top: 2px; font: 12.5px var(--s); color: var(--fg2); text-wrap: pretty; }
  .fix { flex: none; height: 30px; padding: 0 14px; border-radius: var(--srad); background: var(--dangerbtn);
         color: var(--dangerbtnfg); font: 600 13px var(--s); }
  .fix:hover { filter: brightness(1.08); }
  @container (max-width: 560px) { .pb { flex-wrap: wrap; } .fix { margin-left: 32px; } }
</style>

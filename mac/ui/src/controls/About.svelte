<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // About page body: logo, name, tagline and version (no repo link on the Mac: Wind for Mac is private).
  import { iconSvg } from '../design/icons.js';
  let { version = '' } = $props();
</script>

<div class="about">
  <div class="logo">{@html iconSvg('logo', 72)}</div>
  <div class="name">Wind</div>
  <p class="tag">Barely there. Everywhere.</p>
  {#if version}<p class="ver">v{version}</p>{/if}
  <p class="ask">Wind for Mac</p>
</div>

<style>
  .about { padding: 40px 0 48px; text-align: center; display: flex; flex-direction: column; align-items: center; }
  .logo { color: var(--fg); margin-bottom: 14px; }
  .logo :global(svg) { stroke-width: 1.1; }
  .name { font: 600 28px var(--s); letter-spacing: -.3px; margin-bottom: 4px; }
  .tag { color: var(--fg2); margin: 0 0 4px; font-size: 14px; }
  .ver { color: var(--fg3); font: 12.5px var(--m); margin: 0 0 8px; }
  .ask { color: var(--fg3); font-size: 12.5px; margin: 12px 0 0; }
</style>

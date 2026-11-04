<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // Dev/test harness for the regrouped pages: every group's cards rendered with SettingRow and fake
  // values, no bridge. Served at /controls.html (not part of the production build).
  // ?group=<id> picks the group (default hotkeys), ?palette=<id> the built-in theme (default
  // grey), ?engine=<draw|zoom> the engine.
  import '../design/tokens.css';
  import '$shared/design/themes.css';
  import { normalizePalette } from '$shared/design/themes.js';
  import Card from '$shared/shell/Card.svelte';
  import SettingRow from './SettingRow.svelte';
  import { groups, schemaDefaults } from '../settings-schema.js';

  const q = new URLSearchParams(location.search);
  const palette = normalizePalette(q.get('palette'));
  const group = groups.find((g) => g.id === q.get('group')) || groups[0];
  const calls = (window.__calls = []);

  let values = $state({ ...schemaDefaults(), engine: q.get('engine') || 'zoom' });
  const visible = (r) => !r.showIf || (r.showIf.ne !== undefined ? String(values[r.showIf.key]) !== String(r.showIf.ne) : String(values[r.showIf.key]) === String(r.showIf.eq));
  const set = (key, val) => { values[key] = val; calls.push([key, val]); };
  const extra = {
    version: '0.1.0', onAction: (a) => calls.push(['action', a]),
    mac: { accessibility: true, screenRecording: false, zoomShortcuts: ['Scroll gesture'], loginItem: false, logLevel: 1 },
    onMacAction: (a, v) => calls.push(['mac', a, v]),
  };
</script>

<div class="wnd page" data-palette={palette}>
  <h1>{group.label}</h1>
  {#each group.cards as card, i (i)}
    <Card caption={card.caption}>
      {#each card.rows.filter(visible) as r (r.key)}
        <SettingRow row={r} value={values[r.key]} {values} {extra}
                    onChange={(v) => set(r.key, v)} onSet={set} live={(p) => { for (const k in p) set(k, p[k]); }} />
      {/each}
    </Card>
  {/each}
</div>

<style>
  .page { min-height: 100vh; padding: 24px 40px; max-width: 880px; margin: 0 auto; }
  h1 { font: 600 22px var(--s); margin: 0 0 8px; }
</style>

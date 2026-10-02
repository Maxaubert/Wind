<script>
  // Dev/test harness for the regrouped pages: every group's cards rendered with SettingRow and fake
  // values, no bridge. Served at /controls.html (not part of the production build).
  // ?group=<id> picks the group (default hotkeys), ?palette=<id> the built-in theme (default
  // grey), ?model=<engine> the engine.
  import '../design/tokens.css';
  import '../design/themes.css';
  import { normalizePalette } from '../design/themes.js';
  import Card from '../shell/Card.svelte';
  import SettingRow from './SettingRow.svelte';
  import { groups, groupRows, bindKeys } from '../settings-schema.js';

  const q = new URLSearchParams(location.search);
  const palette = normalizePalette(q.get('palette'));
  const group = groups.find((g) => g.id === q.get('group')) || groups[0];
  const calls = (window.__calls = []);

  let values = $state({ model: q.get('model') || 'hybrid', zoomInVk: '33', zoomInButton: '2', noSwallowApps: 'netflix.exe' });
  for (const r of groups.flatMap(groupRows)) {
    if (!r.key.startsWith('__') && !(r.key in values)) values[r.key] = r.def ?? '';
    for (const k of bindKeys(r)) if (!(k in values)) values[k] = '0';
  }
  const visible = (r) => !r.showIf || String(values[r.showIf.key]) === String(r.showIf.eq);
  const set = (key, val) => { values[key] = val; calls.push([key, val]); };
  const extra = {
    runningModel: 'hybrid', mpoNeedsRestart: false, version: '0.20.2',
    onRestart: () => calls.push('restart'), onAction: (a) => calls.push(['action', a]),
    onRepo: () => calls.push('repo'),
    pick: async () => 'RDR2.exe',
    profiles: { names: ['Default', 'Gaming'], active: 'Gaming',
      onSwitch: (n) => calls.push(['switch', n]), onNew: () => calls.push('new'),
      onDelete: (n) => calls.push(['delete', n]) },
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

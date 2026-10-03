<script>
  // Search results: ONE ranked list, best match first. Each result is the setting's real row (the same
  // SettingRow, with the same value and handlers as on its page), so it is edited right here and
  // clicking it goes nowhere. The tab name sits quietly beside the label.
  import SettingRow from '../controls/SettingRow.svelte';
  let { results = [], query = '', values = {}, extra = {}, live = () => {}, onChange = () => {}, onSet = () => {},
        isDisabled = () => false } = $props();
</script>

<div class="res" role="region" aria-label="Search results">
  <h1>Search</h1>
  <p class="sum">{results.length === 0 ? 'No settings match "' + query + '".' : results.length + (results.length === 1 ? ' setting' : ' settings') + ' match "' + query + '", best first. Change them here, Esc clears.'}</p>
  {#if results.length}
    <div class="list">
      {#each results as r (r.key)}
        <div class="hit" data-hit={r.key}>
          <SettingRow row={r.row} value={values[r.key]} {values} {extra} {live} tab={r.groupLabel}
                      disabled={isDisabled(r.row)} onChange={(v) => onChange(r.key, v)} onSet={onSet} />
        </div>
      {/each}
    </div>
  {/if}
</div>

<style>
  .res { padding-top: 28px; }
  h1 { margin: 0 0 6px; font: 600 24px var(--s); color: var(--fg); }
  .sum { margin: 0 0 18px; color: var(--fg3); font: 13px var(--s); }
  .list { border: 1px solid var(--cardb); background: var(--card); border-radius: var(--rc); box-shadow: var(--cshadow); }
  .hit + .hit { border-top: 1px solid var(--rowline); }
</style>

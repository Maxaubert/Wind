<script>
  // Search results, grouped by settings group. Each result is a button that jumps to its row.
  let { results = [], query = '', onJump = () => {} } = $props();
  const total = $derived(results.reduce((n, g) => n + g.rows.length, 0));
</script>

<div class="res" role="region" aria-label="Search results">
  <h1>Search</h1>
  <p class="sum">{total === 0 ? 'No settings match "' + query + '".' : total + (total === 1 ? ' setting' : ' settings') + ' match "' + query + '". Press Enter to open the first, Esc to clear.'}</p>
  {#each results as g (g.id)}
    <div class="gl">{g.label}</div>
    <div class="list">
      {#each g.rows as r (r.key)}
        <button type="button" class="hit" data-hit={r.key} onclick={() => onJump(r)}>
          <span class="t">{r.label}</span>
          {#if r.caption}<span class="c">{r.caption}</span>{/if}
          {#if r.desc}<span class="d">{r.desc}</span>{/if}
        </button>
      {/each}
    </div>
  {/each}
</div>

<style>
  .res { padding-top: 28px; }
  h1 { margin: 0 0 6px; font: 600 24px var(--s); color: var(--fg); }
  .sum { margin: 0 0 6px; color: var(--fg3); font: 13px var(--s); }
  .gl { margin: 22px 0 10px 2px; font: 500 13px var(--s); letter-spacing: .02em; color: var(--fg3); }
  .list { border: 1px solid var(--cardb); background: var(--card); border-radius: 10px; box-shadow: var(--cshadow); overflow: hidden; }
  .hit { display: grid; grid-template-columns: 1fr auto; gap: 2px 12px; width: 100%; text-align: left;
         padding: 12px 16px; color: var(--fg); }
  .hit + .hit { border-top: 1px solid var(--rowline); }
  .hit:hover { background: var(--hover); }
  .t { font: 500 13.5px var(--s); }
  .c { font: 11px var(--m); color: var(--fg3); align-self: center; }
  .d { grid-column: 1 / -1; font: 12px var(--s); color: var(--fg3); }
</style>

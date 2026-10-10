<script>
  import { iconSvg } from '../design/icons.js';
  // groups: the pages above the divider, bottom: the pages below it ([{ id, label, icon }]). `version` shows beside the row with id 'about'.
  let { groups = [], bottom = [], active = '', version = '', query = '', onSelect = () => {}, onSearch = () => {} } = $props();
</script>

{#snippet row(g)}
  <button type="button" class="it" class:sel={g.id === active} data-g={g.id}
          aria-current={g.id === active ? 'page' : undefined} onclick={() => onSelect(g.id)}>
    <span class="icw">{@html iconSvg(g.icon, 16)}</span>{g.label}
    {#if g.id === 'about' && version}<span class="n">{version}</span>{/if}
  </button>
{/snippet}

<aside class="side">
  <label class="search">
    {@html iconSvg('search', 16)}
    <input type="search" placeholder="Search settings" aria-label="Search settings" value={query}
           oninput={(e) => onSearch(e.currentTarget.value)} />
    <kbd>Ctrl F</kbd>
  </label>
  <nav aria-label="Settings sections">
    {#each groups as g (g.id)}{@render row(g)}{/each}
  </nav>
  <nav class="bottom" aria-label="More">
    {#each bottom as g (g.id)}{@render row(g)}{/each}
  </nav>
</aside>

<style>
  .side { background: var(--side); border-right: 1px solid var(--line); display: flex; flex-direction: column;
          min-height: 0; padding: 12px 10px 14px; overflow-y: auto; }
  .search { height: 32px; border: 1px solid var(--line2); display: flex; align-items: center; gap: 8px;
            padding: 0 6px 0 10px; color: var(--fg3); margin-bottom: 12px; border-radius: var(--srad); }
  .search:focus-within { border-color: var(--fg); }
  .search input { flex: 1; min-width: 0; background: none; border: 0; outline: 0; color: var(--fg);
                  font: 13px var(--nf); padding: 0; -webkit-appearance: none; appearance: none; }
  .search input::placeholder { color: var(--fg3); }
  .search input::-webkit-search-cancel-button { display: none; }
  .search kbd { font: 10px var(--m); border: 1px solid var(--chipb); border-radius: 3px; padding: 1px 5px; color: var(--fg3); }
  nav { display: flex; flex-direction: column; gap: 2px; }
  .it { position: relative; height: 36px; display: flex; align-items: center; gap: 10px; padding: 0 10px;
        color: var(--fg3); font: var(--w) 12.5px var(--nf); border-radius: var(--rad); text-align: left; width: 100%; }
  .icw { width: 20px; display: grid; place-items: center; flex: none; }
  .it:hover { background: var(--hover); color: var(--fg); }
  .it.sel { background: var(--sel); color: var(--selfg); }
  .n { margin-left: auto; font: 400 10.5px var(--m); color: var(--fg3); }   /* right-aligned to the row padding, as in the mockup */
  /* A thin divider, no labels; the bottom group sits right under the list, not pinned to the window bottom. */
  .bottom { margin-top: 10px; padding-top: 8px; border-top: 1px solid var(--line2); }
  /* Narrow window: search on its own row, then the pages as a wrapped strip above the content. */
  @media (max-width: 700px) {
    .side { flex-direction: row; flex-wrap: wrap; align-items: center; gap: 6px; padding: 8px 12px; border-right: 0;
            border-bottom: 1px solid var(--line); overflow-y: visible; }
    .search { flex: 1 1 100%; margin-bottom: 2px; }
    nav { flex-direction: row; flex-wrap: wrap; gap: 2px; }
    .it { width: auto; height: 32px; padding: 0 10px 0 6px; gap: 6px; }
    .bottom { margin-top: 0; padding-top: 0; border-top: 0; flex-basis: 100%; }
  }
</style>

<script>
  import { iconSvg } from '../design/icons.js';
  // groups / expert: [{ id, label, icon }]. `version` shows beside the row with id 'about'.
  let { groups = [], expert = [], active = '', version = '', query = '', onSelect = () => {}, onSearch = () => {} } = $props();
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
  <div class="adv">
    <div class="lbl">Expert</div>
    {#each expert as g (g.id)}{@render row(g)}{/each}
  </div>
</aside>

<style>
  .side { background: var(--side); border-right: 1px solid var(--line); display: flex; flex-direction: column;
          min-height: 0; padding: 12px 10px 14px; overflow-y: auto; scrollbar-width: thin; }
  .search { height: 32px; border: 1px solid var(--line2); display: flex; align-items: center; gap: 8px;
            padding: 0 6px 0 10px; color: var(--fg3); margin-bottom: 12px; border-radius: var(--srad); }
  .search:focus-within { border-color: var(--fg); }
  .search input { flex: 1; min-width: 0; background: none; border: 0; outline: 0; color: var(--fg);
                  font: 13px var(--nf); padding: 0; -webkit-appearance: none; appearance: none; }
  .search input::placeholder { color: var(--fg3); }
  .search input::-webkit-search-cancel-button { display: none; }
  .search kbd { font: 10px var(--m); border: 1px solid var(--chipb); border-radius: 3px; padding: 1px 5px; color: var(--fg3); }
  nav, .adv { display: flex; flex-direction: column; gap: 2px; }
  .it { position: relative; height: 36px; display: flex; align-items: center; gap: 10px; padding: 0 10px;
        color: var(--fg3); font: var(--w) 12.5px var(--nf); border-radius: var(--rad); text-align: left; width: 100%; }
  .icw { width: 20px; display: grid; place-items: center; flex: none; }
  .it:hover { background: var(--hover); color: var(--fg); }
  .it.sel { background: var(--hlGrey); color: var(--fg); }
  .n { margin: 0 18px 0 auto; font: 400 10.5px var(--m); color: var(--fg3); }   /* the mockup's chevron slot: 22px - 6px + 2px */
  .adv { margin-top: auto; border-top: 1px solid var(--line2); padding-top: 8px; }
  .lbl { font: 600 11px var(--m); letter-spacing: .1em; text-transform: uppercase; color: var(--fg3); padding: 2px 10px 8px; }
</style>

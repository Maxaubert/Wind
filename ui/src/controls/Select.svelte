<script>
  // Native select drawn flat to match the cards (chevron, soft fill). Native keeps the keyboard and
  // screen-reader contract for free; the colours come from the tokens.
  let { value = '', options = [], labels = null, onChange = () => {}, disabled = false,
        labelledby, describedby } = $props();
  const show = (o) => (labels && labels[o]) || o;
</script>

<span class="sel" class:disabled>
  <select {disabled} value={String(value)} aria-labelledby={labelledby} aria-describedby={describedby}
          onchange={(e) => onChange(e.currentTarget.value)}>
    {#each options as o (o)}<option value={o}>{show(o)}</option>{/each}
  </select>
  <svg class="chev" viewBox="0 0 10 10" width="10" height="10" aria-hidden="true" focusable="false">
    <path d="M2 3.5 5 6.5 8 3.5" fill="none" stroke="currentColor" stroke-width="1.4"/>
  </svg>
</span>

<style>
  .sel { position: relative; display: inline-flex; align-items: center; color: var(--fg3); }
  .sel.disabled { opacity: .45; }
  select { -webkit-appearance: none; appearance: none; height: 30px; min-width: 150px;
           padding: 0 30px 0 12px; border-radius: var(--srad); border: 1px solid var(--chipb);
           background: var(--chip); color: var(--fg); font: 13px var(--s); cursor: pointer; }
  select:hover { border-color: var(--outline); }
  select:focus-visible { outline: 2px solid var(--fg); outline-offset: 2px; }
  select option { background: var(--card); color: var(--fg); }
  .chev { position: absolute; right: 11px; pointer-events: none; }
</style>

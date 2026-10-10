<script>
  // A captioned group of rows. Children render inside the bordered card.
  let { caption = '', children } = $props();
</script>

{#if caption}<div class="cap">{caption}</div>{/if}
<div class="card">{@render children?.()}</div>

<style>
  .cap { margin: 22px 0 10px 2px; font: 500 13px var(--s); letter-spacing: .02em; color: var(--fg3); }
  :global(.banner) + .cap { margin-top: 24px; }
  .card { container-type: inline-size; border: 1px solid var(--cardb); background: var(--card); border-radius: var(--rc); box-shadow: var(--cshadow); }
  .card :global(.row + .row) { border-top: 1px solid var(--rowline); }
</style>

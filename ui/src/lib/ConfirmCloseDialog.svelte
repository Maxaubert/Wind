<script>
  export let show = false;
  export let onCancel = () => {};
  export let onConfirm = () => {};
  function onKey(e) { if (show && e.key === 'Escape') onCancel(); }
</script>
<svelte:window on:keydown={onKey} />
{#if show}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div class="backdrop" style="app-region:no-drag;-webkit-app-region:no-drag"
       on:click|self={onCancel}>
    <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="confirmCloseTitle">
      <h3 id="confirmCloseTitle">Close Wind?</h3>
      <p>Closing this window will quit Wind entirely. The magnifier will stop until you launch Wind again.</p>
      <div class="actions">
        <button class="btn" on:click={onCancel}>Cancel</button>
        <button class="btn primary" on:click={onConfirm}>Quit Wind</button>
      </div>
    </div>
  </div>
{/if}
<style>
  .backdrop { position: fixed; inset: 0; background: rgba(0,0,0,.55); display: grid;
              place-items: center; z-index: 100; }
  .dialog { background: var(--bg); color: var(--text); border: 1px solid var(--line);
            border-radius: 12px; padding: 22px 24px; max-width: 360px;
            box-shadow: 0 20px 60px rgba(0,0,0,.55); }
  .dialog h3 { margin: 0 0 8px; font-size: 17px; font-weight: 600; }
  .dialog p { margin: 0 0 18px; color: var(--muted); font-size: 13px; line-height: 1.5; }
  .actions { display: flex; gap: 10px; justify-content: flex-end; }
  .btn { padding: 8px 16px; border-radius: 7px; border: 1px solid var(--line); background: transparent;
         color: var(--text); font-size: 13px; cursor: pointer; }
  .btn.primary { background: var(--accent); border-color: var(--accent); color: #fff; }
</style>

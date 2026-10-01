<script>
  // Floating unsaved-changes pill. Renders nothing when there is nothing to save.
  let { count = 0, onSave = () => {}, onDiscard = () => {} } = $props();
</script>

{#if count > 0}
  <div class="capsule" role="status">
    <b>{count} unsaved {count === 1 ? 'change' : 'changes'}</b>
    <button type="button" class="btn g" onclick={() => onDiscard()}>Discard</button>
    <button type="button" class="btn p" onclick={() => onSave()}>Save</button>
  </div>
{/if}

<style>
  .capsule { backdrop-filter: blur(14px); position: absolute; left: 50%; bottom: 26px; transform: translateX(-50%);
             height: 48px; border-radius: 999px; background: var(--pill); border: 1px solid var(--pillb);
             box-shadow: var(--shadow); display: flex; align-items: center; gap: 10px; padding: 0 5px 0 20px; white-space: nowrap; }
  b { font: 600 13px var(--s); }
  .btn { height: 36px; padding: 0 20px; border-radius: 999px; font: 600 13px var(--s); }
  .g { color: var(--fg2); }
  .p { background: var(--accent); color: var(--onaccent); }
</style>

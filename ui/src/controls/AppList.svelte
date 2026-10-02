<script>
  // An exe list kept as one comma-separated ini string. The row shows a summary and a Manage button;
  // the dialog lists the apps with Remove and adds through the host's file picker (`pick`).
  import { pickExe } from '../bridge.js';
  import { dialog } from '../lib/dialog.js';
  let { value = '', title = '', onChange = () => {}, disabled = false, labelledby, describedby, valueId,
        pick = pickExe } = $props();
  let open = $state(false);
  const items = $derived(String(value ?? '').split(',').map((s) => s.trim()).filter(Boolean));
  const summary = $derived(items.length === 0 ? 'None' : items.length === 1 ? items[0] : `${items.length} apps`);
  const uid = 'al-' + Math.random().toString(36).slice(2, 8);
  async function add() {
    const name = await pick();
    if (!name) return;
    // The core matches exe names case-insensitively, so two spellings are one entry.
    if (items.some((i) => i.toLowerCase() === name.toLowerCase())) return;
    onChange([...items, name].join(','));
  }
  const remove = (name) => onChange(items.filter((i) => i !== name).join(','));
</script>

<div class="al">
  <span class="sum" id={valueId}>{summary}</span>
  <button type="button" class="chip" {disabled} id="{uid}-b"
          aria-labelledby="{labelledby ?? ''} {uid}-b" aria-describedby={describedby}
          onclick={() => (open = true)}>Manage</button>
</div>

{#if open}
  <div class="back" role="presentation" onclick={(e) => { if (e.target === e.currentTarget) open = false; }}>
    <div class="box" role="dialog" aria-modal="true" aria-labelledby="{uid}-t" use:dialog={{ onClose: () => (open = false) }}>
      <div class="head">
        <h2 id="{uid}-t">{title}</h2>
        <button type="button" class="x" aria-label="Close" onclick={() => (open = false)}>&#215;</button>
      </div>
      {#if items.length === 0}
        <p class="empty">No apps yet</p>
      {:else}
        <ul>
          {#each items as name (name)}
            <li><span class="nm">{name}</span>
              <button type="button" class="rm" aria-label="Remove {name}" onclick={() => remove(name)}>Remove</button></li>
          {/each}
        </ul>
      {/if}
      <div class="btns"><button type="button" class="chip" data-autofocus onclick={add}>Add program...</button></div>
    </div>
  </div>
{/if}

<style>
  .al { display: inline-flex; align-items: center; gap: 10px; }
  .sum { color: var(--fg3); max-width: 180px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .chip { height: 28px; padding: 0 12px; border-radius: var(--srad); background: var(--chip);
          border: 1px solid var(--chipb); color: var(--fg); font: 13px var(--s); }
  .chip:hover { border-color: var(--outline); }
  .chip:disabled { opacity: .45; cursor: default; }
  .back { position: fixed; inset: 0; background: rgba(0, 0, 0, .5); display: flex; align-items: center;
          justify-content: center; z-index: 50; }
  .box { background: var(--card); color: var(--fg); border: 1px solid var(--cardb); border-radius: var(--rc);
         padding: 18px 20px; width: 400px; max-width: calc(100vw - 32px); box-shadow: var(--shadow);
         font: 13px var(--s); }
  .back { animation: fade var(--dur) var(--ease); }
  .box { animation: pop var(--dur) var(--ease); }
  @keyframes fade { from { opacity: 0; } }
  @keyframes pop { from { opacity: 0; transform: scale(.98); } }
  .head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }
  h2 { margin: 0; font-size: 15px; font-weight: 600; }
  .x { width: 28px; height: 28px; border-radius: var(--srad); color: var(--fg3); font-size: 18px; line-height: 1; }
  .x:hover { background: var(--hover); color: var(--fg); }
  .empty { margin: 0; padding: 30px 0; text-align: center; color: var(--fg3); }
  ul { list-style: none; margin: 0; padding: 0; max-height: 240px; overflow-y: auto; }
  li { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 7px 0;
       border-bottom: 1px solid var(--rowline); }
  li:last-child { border-bottom: 0; }
  .nm { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .rm { color: var(--fg3); padding: 4px 6px; border-radius: var(--srad); }
  .rm:hover { color: var(--fg); background: var(--hover); }
  .btns { display: flex; justify-content: flex-end; margin-top: 16px; }
</style>

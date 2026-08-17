<script>
  // Manage dialog for an exe list (Keybinds -> "Don't swallow keys in these apps"). Inline chips
  // stopped scaling once more than a game or two was listed: they wrapped into the description and
  // squeezed the row. A dialog keeps the row to one button no matter how many programs are listed.
  //
  // Purely presentational: the parent Row owns the value and passes handlers, so this file has no
  // knowledge of the config shape or the bridge.
  export let title, items, onAdd, onRemove, onClose;
  import { dialog } from './dialog.js';
</script>
<!-- Click the backdrop (but not the box) to dismiss, matching the restart dialog in Settings.
     Escape is handled by the `dialog` action on the box (issue #201), not a window listener: the
     action also moves focus INTO the dialog, traps Tab, and hands focus back on close. -->
<div class="backdrop" role="presentation" on:click|self={onClose}>
  <div class="box" role="dialog" aria-modal="true" aria-labelledby="applist-title"
       use:dialog={{ onClose }}>
    <div class="head">
      <h2 id="applist-title">{title}</h2>
      <button class="x" on:click={onClose} title="Close" aria-label="Close">&#215;</button>
    </div>
    {#if items.length === 0}
      <p class="empty">No apps yet</p>
    {:else}
      <ul>
        {#each items as name (name)}
          <li>
            <span class="name">{name}</span>
            <button class="rm" on:click={() => onRemove(name)} aria-label="Remove {name}">Remove</button>
          </li>
        {/each}
      </ul>
    {/if}
    <!-- No confirm button: edits apply as they are made, so there is nothing to accept. Closing is
         the X, Escape, or the backdrop, which is why the one action here is Add. -->
    <div class="btns">
      <button class="primary" data-autofocus on:click={onAdd}>Add program...</button>
    </div>
  </div>
</div>
<style>
  /* Ported from the restart dialog in Settings.svelte (.mbackdrop / .mbox) so both modals match. */
  .backdrop{position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;
            justify-content:center;z-index:50}
  .box{background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:10px;
       padding:20px 22px;width:400px;box-shadow:0 12px 40px rgba(0,0,0,.5)}
  .head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:0 0 14px}
  .box h2{margin:0;font-size:15px}
  /* Caption-bar sizing/hover, matching the window buttons in Settings.svelte. */
  .x{width:28px;height:28px;flex-shrink:0;display:grid;place-items:center;border:0;
     background:transparent;color:var(--muted);font-size:18px;line-height:1;cursor:pointer;
     border-radius:7px}
  .x:hover{background:#e81123;color:#fff}
  .empty{margin:0;padding:34px 0;text-align:center;font-size:13px;color:var(--muted)}
  /* Cap the height so a long list scrolls inside the dialog instead of growing it off-screen. */
  ul{list-style:none;margin:0;padding:0;max-height:240px;overflow-y:auto;
     scrollbar-width:thin;scrollbar-color:var(--track) transparent}
  li{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:7px 0;
     border-bottom:1px solid var(--line)}
  li:last-child{border-bottom:0}
  .name{font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .rm{flex-shrink:0;border:0;background:transparent;color:var(--muted);font-size:12px;
      cursor:pointer;padding:4px 6px;border-radius:6px}
  .rm:hover{color:#e81123;background:var(--hover)}
  .btns{display:flex;justify-content:flex-end;margin-top:18px}
  .btns button{padding:7px 16px;border-radius:7px;font-size:12.5px;cursor:pointer;
               border:1px solid var(--line);background:transparent;color:var(--text)}
  .btns button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
</style>

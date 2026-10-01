<script>
  // One modal prompt. buttons: [{ label, kind: 'primary' | '', onClick }]. Esc calls onEsc (omit to
  // make Esc do nothing). Focus moves into the box and Tab is held there by the dialog action.
  import { dialog } from '../lib/dialog.js';
  let { id = 'p', title = '', text = '', buttons = [], onEsc = undefined } = $props();
</script>

<div class="mbackdrop">
  <div class="mbox" role="dialog" aria-modal="true" aria-labelledby="{id}-t" aria-describedby="{id}-d"
       use:dialog={{ onClose: onEsc }}>
    <h2 id="{id}-t">{title}</h2>
    <p id="{id}-d">{text}</p>
    <div class="mbtns">
      {#each buttons as b (b.label)}
        <button type="button" class:primary={b.kind === 'primary'} onclick={b.onClick}>{b.label}</button>
      {/each}
    </div>
  </div>
</div>

<style>
  .mbackdrop { position: fixed; inset: 0; background: rgba(0, 0, 0, .55); display: flex;
               align-items: center; justify-content: center; z-index: 50; }
  .mbox { background: var(--card); color: var(--fg); border: 1px solid var(--line2); border-radius: 10px;
          padding: 22px 24px; width: 440px; max-width: calc(100vw - 48px); box-shadow: var(--shadow); }
  h2 { margin: 0 0 8px; font: 600 15px var(--s); }
  p { margin: 0 0 20px; font: 13px/1.5 var(--s); color: var(--fg2); }
  .mbtns { display: flex; gap: 8px; justify-content: flex-end; flex-wrap: wrap; }
  button { height: 32px; padding: 0 16px; border-radius: 999px; border: 1px solid var(--chipb);
           background: var(--chip); color: var(--fg); font: 600 12.5px var(--s); }
  button:hover { border-color: var(--outline); }
  button.primary { background: var(--accent); border-color: var(--accent); color: var(--onaccent); }
</style>

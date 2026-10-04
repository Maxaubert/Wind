<script>
  // "New profile" dialog (ia07, buttons per Max 2026-10-04): a name, then Cancel, New (the default settings) or
  // Duplicate current (a copy of the current settings, unsaved changes included). Enter in the name field
  // duplicates, the old default. Props only; the page runs the bridge calls.
  //   names       existing profile names (for the duplicate check and the suggested name)
  //   onCreate({ name, from })   from is 'current' or 'default'      onCancel()
  import { untrack } from 'svelte';
  import { dialog } from '../lib/dialog.js';
  import { nameError, suggestName } from './profileName.js';
  let { names = [], onCreate = () => {}, onCancel = () => {} } = $props();
  let name = $state(untrack(() => suggestName(names)));   // the suggestion is only the starting text
  let error = $state('');
  let input = $state();
  function create(from) {
    const t = name.trim();
    const err = nameError(t, names);
    error = err;
    if (err) { input && input.focus(); return; }
    onCreate({ name: t, from });
  }
  function key(e) {
    if (e.key === 'Enter' && e.target.tagName === 'INPUT') { e.preventDefault(); create('current'); }
  }
</script>

<div class="scrim">
  <div class="dlg np" role="dialog" aria-modal="true" aria-labelledby="np-t" tabindex="-1"
       use:dialog={{ onClose: onCancel }} onkeydown={key}>
    <h2 id="np-t">New profile</h2>
    <label class="fl" for="np-name">Name</label>
    <input id="np-name" type="text" data-autofocus bind:this={input} bind:value={name} maxlength="40" autocomplete="off"
           aria-describedby="np-err" aria-invalid={error ? 'true' : 'false'} oninput={() => (error = '')} />
    <div class="err" id="np-err" role="alert">{error}</div>
    <div class="acts">
      <button type="button" class="btn g" onclick={onCancel}>Cancel</button>
      <span class="sp"></span>
      <button type="button" class="btn" onclick={() => create('default')}>New</button>
      <button type="button" class="btn p" onclick={() => create('current')}>Duplicate current</button>
    </div>
  </div>
</div>

<style>
  .scrim { position: fixed; inset: 0; z-index: 50; background: var(--scrim); display: grid; place-items: center;
           animation: fade var(--dur) var(--ease); }
  .dlg { width: 380px; max-width: calc(100% - 32px); padding: 20px; background: var(--card); color: var(--fg);
         border: 1px solid var(--chipb); border-radius: var(--rad); box-shadow: var(--shadow); animation: pop var(--dur) var(--ease); }
  @keyframes fade { from { opacity: 0; } }
  @keyframes pop { from { opacity: 0; transform: scale(.98); } }
  h2 { margin: 0 0 6px; font: 600 15px var(--s); }
  .fl { display: block; margin: 12px 0 6px; font: 500 12px var(--s); color: var(--fg3); }
  input { width: 100%; height: 32px; padding: 0 10px; background: var(--chip); border: 1px solid var(--chipb);
          border-radius: var(--rad); color: var(--fg); font: 12px var(--m); }
  input[aria-invalid="true"] { border-color: var(--danger); }
  .err { min-height: 16px; margin-top: 6px; color: var(--danger); font: 12px var(--s); }
  .err:empty { margin-top: 0; min-height: 0; }
  .acts { display: flex; align-items: center; gap: 8px; margin-top: 18px; }
  .sp { flex: 1; }
  .btn { height: 32px; padding: 0 16px; border-radius: var(--rp); border: 1px solid var(--chipb); background: var(--chip);
         color: var(--fg); font: 600 12.5px var(--s); white-space: nowrap; }
  .btn:hover { border-color: var(--outline); }
  .btn.g:hover { background: var(--hover); color: var(--fg); }
  .btn.p { background: var(--accent); border-color: var(--accent); color: var(--onaccent); }
</style>

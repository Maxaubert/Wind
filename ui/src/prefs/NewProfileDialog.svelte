<script>
  // "New profile" dialog (ia07): a name, and where the new profile starts from, either a copy of the current
  // settings or the defaults. Props only; the page runs the bridge calls.
  //   names       existing profile names (for the duplicate check and the suggested name)
  //   current     the active profile's name, shown in the first option
  //   onCreate({ name, from })   from is 'current' or 'default'      onCancel()
  import { untrack } from 'svelte';
  import { dialog } from '../lib/dialog.js';
  import { nameError, suggestName } from './profileName.js';
  let { names = [], current = '', onCreate = () => {}, onCancel = () => {} } = $props();
  let name = $state(untrack(() => suggestName(names)));   // the suggestion is only the starting text
  let from = $state('current');
  let error = $state('');
  let input = $state();
  const opts = $derived([['current', 'Current settings (copy of ' + current + ')'], ['default', 'Default settings']]);
  function create() {
    const t = name.trim();
    const err = nameError(t, names);
    error = err;
    if (err) { input && input.focus(); return; }
    onCreate({ name: t, from });
  }
  function pick(id, focus = false) {
    from = id;
    if (focus) setTimeout(() => document.querySelector('.np [role="radio"][aria-checked="true"]')?.focus(), 0);
  }
  function radioKey(e) {
    const d = { ArrowUp: -1, ArrowLeft: -1, ArrowDown: 1, ArrowRight: 1 }[e.key];
    if (!d) return;
    e.preventDefault();
    const i = opts.findIndex((o) => o[0] === from);
    pick(opts[(i + d + opts.length) % opts.length][0], true);
  }
  function key(e) {
    if (e.key === 'Enter' && (e.target.tagName === 'INPUT' || e.target.getAttribute('role') === 'radio')) { e.preventDefault(); create(); }
  }
</script>

<div class="scrim">
  <div class="dlg np" role="dialog" aria-modal="true" aria-labelledby="np-t" tabindex="-1" use:dialog={{ onClose: onCancel }} onkeydown={key}>
    <h2 id="np-t">New profile</h2>
    <label class="fl" for="np-name">Name</label>
    <input id="np-name" type="text" data-autofocus bind:this={input} bind:value={name} maxlength="40" autocomplete="off"
           aria-describedby="np-err" aria-invalid={error ? 'true' : 'false'} oninput={() => (error = '')} />
    <div class="err" id="np-err" role="alert">{error}</div>
    <div class="fl" id="np-from">Start from</div>
    <div class="opts" role="radiogroup" tabindex="-1" aria-labelledby="np-from" onkeydown={radioKey}>
      {#each opts as [id, label] (id)}
        <button type="button" role="radio" aria-checked={from === id} tabindex={from === id ? 0 : -1} onclick={() => pick(id, true)}>{label}</button>
      {/each}
    </div>
    <div class="acts">
      <button type="button" class="btn g" onclick={onCancel}>Cancel</button>
      <button type="button" class="btn p" onclick={create}>Create</button>
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
  .fl { display: block; margin: 14px 0 6px; font: 500 12px var(--s); color: var(--fg3); }
  h2 + .fl { margin-top: 12px; }
  input { width: 100%; height: 32px; padding: 0 10px; background: var(--chip); border: 1px solid var(--chipb);
          border-radius: var(--rad); color: var(--fg); font: 12px var(--m); }
  input[aria-invalid="true"] { border-color: var(--danger); }
  .err { min-height: 16px; margin-top: 6px; color: var(--danger); font: 12px var(--s); }
  .err:empty { margin-top: 0; min-height: 0; }
  .opts { display: flex; flex-direction: column; border: 1px solid var(--chipb); border-radius: var(--rad); overflow: hidden; }
  .opts button { height: 30px; padding: 0 12px; text-align: left; background: var(--chip); color: var(--fg3); font: 12px var(--s); }
  .opts button + button { border-top: 1px solid var(--chipb); }
  .opts button:hover { color: var(--fg); }
  .opts button[aria-checked="true"] { background: var(--sel); color: var(--selfg); font-weight: 600; }
  .opts button:focus-visible { outline-offset: -2px; }
  .acts { display: flex; justify-content: flex-end; gap: 8px; margin-top: 18px; }
  .btn { height: 32px; padding: 0 18px; border-radius: var(--rp); border: 1px solid var(--chipb); background: var(--chip);
         color: var(--fg); font: 600 12.5px var(--s); }
  .btn:hover { border-color: var(--outline); }
  .btn.g:hover { background: var(--hover); color: var(--fg); }
  .btn.p { background: var(--accent); border-color: var(--accent); color: var(--onaccent); }
</style>

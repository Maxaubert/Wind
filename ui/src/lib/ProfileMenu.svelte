<script>
  // Titlebar profile dropdown: click the trigger to list profiles (active checked) with
  // "Create new profile" at the bottom. Right-click a row (or its "..." button) for
  // Rename / Duplicate / Delete. Create and Rename share the inline name editor. All mutations
  // are delegated to onAction; the parent owns the bridge calls, dirty guard, and list state.
  export let active = '';
  export let names = [];
  export let onAction = () => {};

  let open = false;
  let ctxFor = '';            // profile name whose context menu is showing ('' = none)
  let editMode = '';          // '' | 'create' | 'rename'
  let editFrom = '';          // rename source
  let editName = '';
  let editError = '';
  let confirmDelete = '';     // profile name pending delete confirmation ('' = none)
  let inputEl;

  const forbidden = /[\\/:*?"<>|]/;
  const reserved = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i;
  function nameError(n, allowSelf = '') {
    const t = n;
    if (!t) return 'Name cannot be empty';
    if (t.length > 40) return 'Name is too long (max 40 characters)';
    if (forbidden.test(t) || [...t].some(c => c.charCodeAt(0) < 32)) return 'Name contains a forbidden character';
    if (t !== t.trim() || t.startsWith('.') || t.endsWith('.')) return 'Name cannot start/end with a space or dot';
    if (reserved.test(t)) return 'That name is reserved by Windows';
    const clash = names.some(x => x.toLowerCase() === t.toLowerCase() &&
                                  x.toLowerCase() !== allowSelf.toLowerCase());
    if (clash) return 'A profile with that name already exists';
    return '';
  }
  function toggle() { open = !open; if (!open) reset(); }
  function reset() { ctxFor = ''; editMode = ''; editName = ''; editError = ''; confirmDelete = ''; }
  function startCreate() { editMode = 'create'; editName = ''; editError = ''; ctxFor = ''; focusSoon(); }
  function startRename(n) { editMode = 'rename'; editFrom = n; editName = n; editError = ''; ctxFor = ''; focusSoon(); }
  function focusSoon() { setTimeout(() => inputEl && inputEl.focus(), 0); }
  function commitEdit() {
    const err = nameError(editName, editMode === 'rename' ? editFrom : '');
    if (err) { editError = err; return; }
    if (editMode === 'create') onAction('create', { name: editName });
    else onAction('rename', { from: editFrom, to: editName });
    open = false; reset();
  }
  function pick(n) {
    if (n.toLowerCase() === active.toLowerCase()) { open = false; reset(); return; }
    onAction('switch', { name: n });
    open = false; reset();
  }
  function doDelete(n) { onAction('delete', { name: n }); open = false; reset(); }
  function clickOutside(node) {
    const h = (e) => { if (!node.contains(e.target)) { open = false; reset(); } };
    document.addEventListener('mousedown', h, true);
    return { destroy: () => document.removeEventListener('mousedown', h, true) };
  }
</script>

<div class="pmwrap" use:clickOutside style="app-region:no-drag;-webkit-app-region:no-drag">
  <button class="ptrigger" on:click={toggle} aria-haspopup="menu" aria-expanded={open}>
    <span class="pname">{active || 'Profiles'}</span>
    <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden="true"><path d="M2 3.5 5 6.5 8 3.5" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/></svg>
  </button>
  {#if open}
    <div class="pmenu" role="menu">
      {#each names as n (n)}
        <div class="prow" class:activerow={n.toLowerCase() === active.toLowerCase()}
             role="menuitem" tabindex="0"
             on:click={() => pick(n)}
             on:keydown={(e) => { if (e.key === 'Enter' && e.target === e.currentTarget) { e.preventDefault(); pick(n); } }}
             on:contextmenu|preventDefault={() => { ctxFor = ctxFor === n ? '' : n; confirmDelete = ''; }}>
          <span class="pcheck">{#if n.toLowerCase() === active.toLowerCase()}&#10003;{/if}</span>
          <span class="plabel">{n}</span>
          <button class="pdots" title="Profile actions" aria-label="Profile actions for {n}"
                  on:click|stopPropagation={() => { ctxFor = ctxFor === n ? '' : n; confirmDelete = ''; }}>&#8943;</button>
        </div>
        {#if ctxFor === n}
          <div class="pctx" role="menu">
            {#if confirmDelete === n}
              <div class="pconfirm">
                <span>Delete "{n}"?</span>
                <button class="pact danger" on:click={() => doDelete(n)}>Delete</button>
                <button class="pact" on:click={() => (confirmDelete = '')}>Cancel</button>
              </div>
            {:else}
              <button class="pact" on:click={() => startRename(n)}>Rename</button>
              <button class="pact" on:click={() => { onAction('duplicate', { name: n }); ctxFor = ''; }}>Duplicate</button>
              <button class="pact danger" disabled={names.length <= 1}
                      title={names.length <= 1 ? 'The last profile cannot be deleted' : ''}
                      on:click={() => (confirmDelete = n)}>Delete</button>
            {/if}
          </div>
        {/if}
      {/each}
      <div class="psep"></div>
      {#if editMode}
        <div class="pedit">
          <input bind:this={inputEl} bind:value={editName} maxlength="40"
                 placeholder={editMode === 'create' ? 'New profile name' : 'New name'}
                 on:keydown={(e) => { if (e.key === 'Enter') commitEdit(); if (e.key === 'Escape') reset(); }} />
          <button class="pact" on:click={commitEdit}>{editMode === 'create' ? 'Create' : 'Rename'}</button>
          {#if editError}<div class="perr">{editError}</div>{/if}
        </div>
      {:else}
        <button class="prow pcreate" on:click={startCreate}>Create new profile&#8230;</button>
      {/if}
    </div>
  {/if}
</div>

<svelte:window on:keydown={(e) => {
  // Escape closes like every other popup: first press cancels an in-progress edit, next closes.
  if (!open || e.key !== 'Escape') return;
  e.preventDefault();
  if (editMode || ctxFor || confirmDelete) reset();
  else { open = false; reset(); }
}} />

<style>
  .pmwrap { position: relative; margin-left: 10px; }
  .ptrigger { display: inline-flex; align-items: center; gap: 6px; border: 0; background: transparent;
              color: var(--muted); font-size: 12.5px; font-weight: 500; cursor: pointer;
              padding: 4px 8px; border-radius: 6px; }
  .ptrigger:hover { background: var(--hover); color: var(--text); }
  .pmenu { position: absolute; top: 30px; left: 0; min-width: 220px; z-index: 40;
           background: var(--bg, #fff); color: var(--text); border: 1px solid var(--hover);
           border-radius: 8px; box-shadow: 0 8px 28px rgba(0,0,0,.25); padding: 6px; }
  .prow { display: flex; align-items: center; gap: 6px; width: 100%; padding: 6px 8px;
          border-radius: 6px; cursor: pointer; border: 0; background: transparent;
          color: var(--text); font-size: 13px; text-align: left; }
  .prow:hover { background: var(--hover); }
  .pcheck { width: 14px; }
  .plabel { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .pdots { border: 0; background: transparent; color: var(--muted); cursor: pointer;
           border-radius: 4px; padding: 0 4px; }
  .pdots:hover { background: var(--hover); color: var(--text); }
  .pctx { display: flex; gap: 4px; padding: 2px 8px 6px 28px; }
  .pact { border: 1px solid var(--hover); background: transparent; color: var(--text);
          border-radius: 6px; padding: 3px 8px; font-size: 12px; cursor: pointer; }
  .pact:hover:not(:disabled) { background: var(--hover); }
  .pact:disabled { opacity: .45; cursor: default; }
  .pact.danger { color: #e05656; }
  .pconfirm { display: flex; align-items: center; gap: 6px; font-size: 12px; }
  .psep { height: 1px; background: var(--hover); margin: 4px 2px; }
  .pcreate { color: var(--muted); }
  .pedit { padding: 4px 6px; display: flex; gap: 6px; flex-wrap: wrap; }
  .pedit input { flex: 1; min-width: 120px; font-size: 12.5px; padding: 4px 6px;
                 border: 1px solid var(--hover); border-radius: 6px; background: transparent;
                 color: var(--text); }
  .perr { width: 100%; color: #e05656; font-size: 11.5px; padding: 2px 8px; }
</style>

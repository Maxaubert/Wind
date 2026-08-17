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
  let inputEl;
  const uid = 'pm-' + Math.random().toString(36).slice(2, 8);

  // "Default" is the seeded home profile and can never be deleted (host enforces this too).
  const isDefault = (n) => n.toLowerCase() === 'default';

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
  function toggle() { open = !open; if (open) focusMenuSoon(); else reset(); }
  function reset() { ctxFor = ''; editMode = ''; editName = ''; editError = ''; }
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
  // Menu keyboard navigation (issue #201). role="menu" carries a contract - arrows move between
  // items, Home/End jump - and none of it existed: the rows were divs with tabindex=0 that answered
  // only to Enter, and each one WRAPPED a button, which is invalid (a menuitem cannot contain an
  // interactive child). Rows and their "..." buttons are now siblings, both real menuitems, moved
  // between with a roving tabindex.
  let menuEl;
  function menuItems() {
    return menuEl ? [...menuEl.querySelectorAll('[role^="menuitem"]:not([disabled])')] : [];
  }
  function menuKey(e) {
    const items = menuItems();
    if (!items.length) return;
    const i = items.indexOf(document.activeElement);
    let next = null;
    if (e.key === 'ArrowDown')    next = i < 0 ? 0 : (i + 1) % items.length;
    else if (e.key === 'ArrowUp') next = i < 0 ? items.length - 1 : (i - 1 + items.length) % items.length;
    else if (e.key === 'Home')    next = 0;
    else if (e.key === 'End')     next = items.length - 1;
    if (next === null) return;
    e.preventDefault();
    items[next].focus();
  }
  // Opening with the keyboard has to land focus in the menu, or Tab would walk into the page behind.
  function focusMenuSoon() { setTimeout(() => { const i = menuItems()[0]; if (i) i.focus(); }, 0); }
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
    <!-- tabindex="-1" on the container: focus lives on the items (roving), never on the menu box,
         but the menu must stay programmatically focusable. -->
    <div class="pmenu" role="menu" aria-label="Profiles" tabindex="-1"
         bind:this={menuEl} on:keydown={menuKey}>
      {#each names as n (n)}
        <div class="pitem">
          <!-- menuitemradio, not menuitem: exactly one profile is active, and the tick beside it
               was a sighted-only signal before. aria-checked exposes the same fact. -->
          <button class="prow" type="button" class:activerow={n.toLowerCase() === active.toLowerCase()}
                  role="menuitemradio" aria-checked={n.toLowerCase() === active.toLowerCase()}
                  tabindex="-1"
                  on:click={() => pick(n)}
                  on:contextmenu|preventDefault={() => { ctxFor = ctxFor === n ? '' : n; }}>
            <span class="pcheck" aria-hidden="true">{#if n.toLowerCase() === active.toLowerCase()}&#10003;{/if}</span>
            <span class="plabel">{n}</span>
          </button>
          <button class="pdots" type="button" role="menuitem" tabindex="-1"
                  title="Profile actions" aria-label="Profile actions for {n}"
                  aria-haspopup="menu" aria-expanded={ctxFor === n}
                  on:click|stopPropagation={() => { ctxFor = ctxFor === n ? '' : n; }}>&#8943;</button>
          {#if ctxFor === n}
            <!-- Flyout to the RIGHT of the row, absolutely positioned: it overlays instead of
                 reflowing, so opening it can never change the dropdown's width or height. -->
            <div class="pctx" role="menu" aria-label="Profile actions for {n}">
              <button class="pact" type="button" role="menuitem" on:click={() => startRename(n)}>Rename</button>
              <button class="pact" type="button" role="menuitem" on:click={() => { onAction('duplicate', { name: n }); ctxFor = ''; }}>Duplicate</button>
              <button class="pact danger" type="button" role="menuitem" disabled={names.length <= 1 || isDefault(n)}
                      title={isDefault(n) ? 'The Default profile cannot be deleted'
                             : names.length <= 1 ? 'The last profile cannot be deleted' : ''}
                      on:click={() => doDelete(n)}>Delete</button>
            </div>
          {/if}
        </div>
      {/each}
      <div class="psep"></div>
      {#if editMode}
        <div class="pedit">
          <!-- The error belongs to the field (aria-describedby + aria-invalid + role=alert), so it
               is read while focus is still in the input instead of being an orphaned red line. -->
          <label class="sr-only" for="{uid}-name">{editMode === 'create' ? 'New profile name' : 'New name'}</label>
          <input id="{uid}-name" bind:this={inputEl} bind:value={editName} maxlength="40"
                 autocomplete="off" spellcheck="false"
                 aria-invalid={editError ? 'true' : undefined}
                 aria-describedby={editError ? uid + '-err' : undefined}
                 placeholder={editMode === 'create' ? 'New profile name' : 'New name'}
                 on:keydown={(e) => { if (e.key === 'Enter') commitEdit(); if (e.key === 'Escape') reset(); }} />
          <button class="pact" type="button" on:click={commitEdit}>{editMode === 'create' ? 'Create' : 'Rename'}</button>
          {#if editError}<div class="perr" id="{uid}-err" role="alert">{editError}</div>{/if}
        </div>
      {:else}
        <button class="prow pcreate" type="button" role="menuitem" tabindex="-1"
                on:click={startCreate}>Create new profile&#8230;</button>
      {/if}
    </div>
  {/if}
</div>

<svelte:window on:keydown={(e) => {
  // Escape closes like every other popup: first press cancels an in-progress edit, next closes.
  if (!open || e.key !== 'Escape') return;
  e.preventDefault();
  if (editMode || ctxFor) reset();
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
           border-radius: 8px; box-shadow: 0 8px 28px rgba(0,0,0,.25); padding: 6px;
           box-sizing: border-box; }
  /* border-box everywhere in the menu: width:100% + padding must not overflow the container */
  .prow, .pedit, .pedit input, .pctx, .pact { box-sizing: border-box; }
  .prow { display: flex; align-items: center; gap: 6px; width: 100%; padding: 6px 8px;
          border-radius: 6px; cursor: pointer; border: 0; background: transparent;
          color: var(--text); font-size: 13px; text-align: left; font-family: inherit; }
  /* Room for the overlaid "..." so a long profile name cannot run under it. */
  .pitem > .prow { padding-right: 26px; }
  .prow:hover { background: var(--hover); }
  /* The "..." is a sibling now, not a child, so hovering it no longer hovered the row underneath.
     Hover the item and the row lights up either way, as it did before. */
  .pitem:hover > .prow { background: var(--hover); }
  .pcheck { width: 14px; }
  .plabel { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  /* Absolutely placed over the row: the "..." used to be nested INSIDE it, which is invalid inside
     a menuitem. Sibling + overlay keeps the layout pixel-identical. */
  /* right:8px matches the row's own padding, so the glyph sits exactly where it did when it was
     the row's last flex child. */
  .pdots { position: absolute; right: 8px; top: 50%; transform: translateY(-50%);
           border: 0; background: transparent; color: var(--muted); cursor: pointer;
           border-radius: 4px; padding: 0 4px; line-height: 1; }
  .pdots:hover { background: var(--hover); color: var(--text); }
  .pitem { position: relative; }
  /* Flyout submenu: overlays to the right of the row (its own panel, like .pmenu), so it never
     reflows the dropdown. */
  .pctx { position: absolute; left: calc(100% + 2px); top: 0; z-index: 41; min-width: 130px;
          display: flex; flex-direction: column; gap: 2px; padding: 4px;
          background: var(--bg, #fff); border: 1px solid var(--hover); border-radius: 8px;
          box-shadow: 0 8px 28px rgba(0,0,0,.25); box-sizing: border-box; }
  .pact { border: 0; background: transparent; color: var(--text); width: 100%; text-align: left;
          border-radius: 6px; padding: 5px 8px; font-size: 12.5px; cursor: pointer;
          box-sizing: border-box; }
  .pact:hover:not(:disabled) { background: var(--hover); }
  .pact:disabled { opacity: .45; cursor: default; }
  .pact.danger { color: #e05656; }
  .psep { height: 1px; background: var(--hover); margin: 4px 2px; }
  .pcreate { color: var(--muted); }
  .pedit { padding: 4px 6px; display: flex; gap: 6px; flex-wrap: wrap; }
  .pedit input { flex: 1; min-width: 120px; font-size: 12.5px; padding: 4px 6px;
                 border: 1px solid var(--hover); border-radius: 6px; background: transparent;
                 color: var(--text); }
  .perr { width: 100%; color: #e05656; font-size: 11.5px; padding: 2px 8px; }
</style>

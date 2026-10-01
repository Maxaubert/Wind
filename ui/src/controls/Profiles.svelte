<script>
  // Profile management inside the General page (moved from the old title bar). Props only: the page
  // owns the bridge calls. onSwitch(name), onCreate(name), onRename(from, to), onDuplicate(name),
  // onDelete(name). "Default" is the seeded home profile: it can be switched to but never renamed or
  // deleted (the host enforces this too). Delete asks once more inline.
  let { names = [], active = '', onSwitch = () => {}, onCreate = () => {}, onRename = () => {},
        onDuplicate = () => {}, onDelete = () => {}, disabled = false, labelledby, describedby } = $props();
  let mode = $state('');          // '' | 'create' | 'rename' | 'duplicate' | 'delete'
  let draft = $state('');
  let error = $state('');
  let input = $state();
  const isDefault = $derived(active.toLowerCase() === 'default');
  const forbidden = /[\\/:*?"<>|]/;
  const reserved = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i;

  function nameError(t, self = '') {
    if (!t) return 'Name cannot be empty';
    if (t.length > 40) return 'Name is too long (max 40 characters)';
    if (forbidden.test(t) || [...t].some((c) => c.charCodeAt(0) < 32)) return 'Name contains a forbidden character';
    if (t !== t.trim() || t.startsWith('.') || t.endsWith('.')) return 'Name cannot start or end with a space or dot';
    if (reserved.test(t)) return 'That name is reserved by Windows';
    if (names.some((x) => x.toLowerCase() === t.toLowerCase() && x.toLowerCase() !== self.toLowerCase()))
      return 'A profile with that name already exists';
    return '';
  }
  function begin(m) {
    mode = m; error = '';
    draft = m === 'rename' ? active : m === 'duplicate' ? active + ' copy' : '';
    setTimeout(() => input && input.focus(), 0);
  }
  function cancel() { mode = ''; error = ''; draft = ''; }
  function commit() {
    if (mode === 'delete') { onDelete(active); cancel(); return; }
    const err = nameError(draft, mode === 'rename' ? active : '');
    if (err) { error = err; return; }
    if (mode === 'create') onCreate(draft);
    else if (mode === 'rename') onRename(active, draft);
    else if (mode === 'duplicate') onDuplicate(draft);
    cancel();
  }
  function key(e) {
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); cancel(); }
    else if (e.key === 'Enter') { e.preventDefault(); commit(); }
  }
</script>

<div class="pf">
  <span class="sel" class:disabled>
    <select {disabled} value={active} aria-labelledby={labelledby} aria-describedby={describedby}
            onchange={(e) => onSwitch(e.currentTarget.value)}>
      {#each names as n (n)}<option value={n}>{n}</option>{/each}
    </select>
    <svg class="chev" viewBox="0 0 10 10" width="10" height="10" aria-hidden="true" focusable="false">
      <path d="M2 3.5 5 6.5 8 3.5" fill="none" stroke="currentColor" stroke-width="1.4"/>
    </svg>
  </span>
  <div class="acts">
    <button type="button" class="chip" {disabled} onclick={() => begin('create')}>New</button>
    <button type="button" class="chip" {disabled} onclick={() => begin('duplicate')}>Duplicate</button>
    <button type="button" class="chip" disabled={disabled || isDefault} onclick={() => begin('rename')}>Rename</button>
    <button type="button" class="chip" disabled={disabled || isDefault} onclick={() => begin('delete')}>Delete</button>
  </div>
  {#if mode}
    <div class="ed" role="group" aria-label={mode === 'delete' ? 'Confirm delete' : 'Edit profile'} onkeydown={key}>
      {#if mode === 'delete'}
        <span class="q">Delete profile {active}?</span>
      {:else}
        <input bind:this={input} bind:value={draft} type="text" maxlength="40" aria-label="Profile name"
               aria-invalid={error ? 'true' : undefined} />
      {/if}
      <button type="button" class="chip go" onclick={commit}>{mode === 'delete' ? 'Delete' : 'OK'}</button>
      <button type="button" class="chip" onclick={cancel}>Cancel</button>
      {#if error}<span class="err" role="alert">{error}</span>{/if}
    </div>
  {/if}
</div>

<style>
  .pf { display: flex; flex-direction: column; align-items: flex-end; gap: 8px; }
  .acts, .ed { display: flex; flex-wrap: wrap; justify-content: flex-end; align-items: center; gap: 8px; }
  .sel { position: relative; display: inline-flex; align-items: center; color: var(--fg3); }
  .sel.disabled { opacity: .45; }
  select { -webkit-appearance: none; appearance: none; height: 30px; min-width: 150px; padding: 0 30px 0 12px;
           border-radius: var(--srad); border: 1px solid var(--chipb); background: var(--chip); color: var(--fg);
           font: 13px var(--s); cursor: pointer; }
  select:focus-visible { outline: 2px solid var(--fg); outline-offset: 2px; }
  select option { background: var(--card); color: var(--fg); }
  .chev { position: absolute; right: 11px; pointer-events: none; }
  .chip { height: 28px; padding: 0 12px; border-radius: var(--srad); background: var(--chip);
          border: 1px solid var(--chipb); color: var(--fg); font: 13px var(--s); }
  .chip:hover:not(:disabled) { border-color: var(--outline); }
  .chip:disabled { opacity: .4; cursor: default; }
  .chip.go { background: var(--accent); border-color: var(--accent); color: var(--onaccent); font-weight: 600; }
  input { height: 28px; width: 180px; padding: 0 10px; border-radius: var(--srad); border: 1px solid var(--chipb);
          background: var(--chip); color: var(--fg); font: 13px var(--s); }
  input:focus-visible { outline: 2px solid var(--fg); outline-offset: 2px; }
  .q { color: var(--fg2); }
  .err { flex-basis: 100%; text-align: right; color: var(--fg2); font-size: 12px; }
</style>

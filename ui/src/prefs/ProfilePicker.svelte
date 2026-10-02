<script>
  // Profile row: a dropdown of the profiles plus one New button (ia07 mockup). Each profile in the open
  // list carries a trash icon that asks to delete it; with one profile left there is no trash at all, and
  // "Default" (the seeded home profile, protected by the host) never shows one. Props only: the page owns
  // the dialogs and the bridge calls.
  //   names, active   the profile list and the live one
  //   onSwitch(name)  onNew()  onDelete(name)
  import { iconSvg } from '../design/icons.js';
  let { names = [], active = '', onSwitch = () => {}, onNew = () => {}, onDelete = () => {}, disabled = false,
        labelledby, describedby } = $props();
  let open = $state(false);
  let wrap = $state();
  const one = $derived(names.length < 2);
  const canDelete = (n) => !one && n.toLowerCase() !== 'default';
  const opts = () => (wrap ? [...wrap.querySelectorAll('.opt')] : []);

  function set(v, focusTrigger = false) {
    open = v;
    if (v) setTimeout(() => { const o = opts(); (o.find((x) => x.getAttribute('aria-selected') === 'true') || o[0])?.focus(); }, 0);
    else if (focusTrigger) wrap?.querySelector('.trig')?.focus();
  }
  function pick(n) {
    open = false;
    wrap?.querySelector('.trig')?.focus();
    if (n.toLowerCase() !== active.toLowerCase()) onSwitch(n);
  }
  function remove(n) { open = false; onDelete(n); }
  function trigKey(e) {
    if (!open && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) { e.preventDefault(); set(true); }
    else if (open && e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); set(false, true); }
  }
  function listKey(e) {
    const o = opts();
    const cur = o.indexOf(e.target.closest('.item')?.querySelector('.opt'));
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      o[(Math.max(cur, 0) + (e.key === 'ArrowDown' ? 1 : -1) + o.length) % o.length]?.focus();
    } else if (e.key === 'Home' || e.key === 'End') {
      e.preventDefault(); o[e.key === 'Home' ? 0 : o.length - 1]?.focus();
    } else if (e.key === 'Escape') {
      e.preventDefault(); e.stopPropagation(); set(false, true);
    }
  }
  function focusOut(e) {
    if (!open) return;
    if (wrap && e.relatedTarget && wrap.contains(e.relatedTarget)) return;
    setTimeout(() => { const a = document.activeElement; if (open && !(a && wrap && wrap.contains(a))) open = false; }, 0);
  }
</script>

<svelte:document onpointerdown={(e) => { if (open && wrap && !wrap.contains(e.target)) open = false; }} />

<div class="pf">
  <div class="wrap" bind:this={wrap} onfocusout={focusOut}>
    <button type="button" class="trig" {disabled} aria-haspopup="listbox" aria-expanded={open}
            aria-label={'Profile: ' + active} aria-describedby={describedby} onkeydown={trigKey} onclick={() => set(!open)}>{active}</button>
    {#if open}
      <div class="list" role="listbox" aria-label="Profiles" tabindex="-1" onkeydown={listKey}>
        {#each names as n (n)}
          <div class="item">
            <button type="button" class="opt" role="option" tabindex="-1" aria-selected={n.toLowerCase() === active.toLowerCase()}
                    onclick={() => pick(n)}>
              <span class="ck">{#if n.toLowerCase() === active.toLowerCase()}{@html iconSvg('check', 14)}{/if}</span>
              <span class="nm">{n}</span>
            </button>
            {#if canDelete(n)}
              <button type="button" class="del" aria-label={'Delete profile ' + n} onclick={() => remove(n)}>{@html iconSvg('trash', 14)}</button>
            {/if}
          </div>
        {/each}
      </div>
    {/if}
  </div>
  <button type="button" class="chip" {disabled} onclick={onNew}>New</button>
</div>

<style>
  .pf { display: flex; align-items: center; gap: 8px; }
  .wrap { position: relative; }
  .trig { height: 30px; min-width: 150px; padding: 0 32px 0 12px; text-align: left; background: var(--chip);
          border: 1px solid var(--chipb); border-radius: var(--rad); color: var(--fg2); font: 12px var(--m);
          cursor: pointer; display: inline-flex; align-items: center; max-width: 230px; overflow: hidden; white-space: nowrap;
          text-overflow: ellipsis; position: relative; }
  .trig::after { content: ""; position: absolute; right: 10px; top: 50%; width: 7px; height: 7px; margin-top: -6px;
                 border-right: 1.5px solid var(--fg3); border-bottom: 1.5px solid var(--fg3); transform: rotate(45deg); }
  .trig:hover:not(:disabled), .trig[aria-expanded="true"] { color: var(--fg); border-color: var(--outline); }
  .trig:disabled { opacity: .45; cursor: default; }
  .list { position: absolute; top: 34px; right: 0; min-width: 100%; width: 230px; padding: 4px; background: var(--card);
          border: 1px solid var(--chipb); border-radius: var(--rad); box-shadow: var(--shadow); z-index: 6;
          display: flex; flex-direction: column; gap: 1px; }
  .item { display: flex; align-items: center; border-radius: var(--srad); }
  .item:hover, .item:focus-within { background: var(--hover); }
  .opt { flex: 1; min-width: 0; height: 30px; display: flex; align-items: center; gap: 8px; padding: 0 8px; text-align: left;
         border-radius: var(--srad); color: var(--fg2); font: 12px var(--m); }
  .opt:focus-visible { outline-offset: -2px; }
  .item:hover .opt, .opt[aria-selected="true"] { color: var(--fg); }
  .ck { width: 14px; height: 14px; display: grid; place-items: center; flex: none; color: var(--fill); }
  .ck :global(svg) { stroke-width: 1.75; }
  .nm { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .del { width: 26px; height: 26px; margin-right: 2px; display: grid; place-items: center; flex: none; border-radius: var(--srad);
         color: var(--fg3); opacity: .4; }
  .item:hover .del, .item:focus-within .del { opacity: 1; }
  .del:hover { color: var(--danger); background: var(--dangerbg); opacity: 1; }
  .del:focus-visible { outline-offset: -2px; }
  .chip { height: 28px; padding: 0 12px; border-radius: var(--srad); background: var(--chip);
          border: 1px solid var(--chipb); color: var(--fg); font: 13px var(--s); }
  .chip:hover:not(:disabled) { border-color: var(--outline); }
  .chip:disabled { opacity: .4; cursor: default; }
</style>

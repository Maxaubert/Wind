<!-- Copyright (c) 2026 Max Aubert. All rights reserved.
     Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary -->
<script>
  // A native select drawn flat to match the cards (chevron, soft fill), whose list opens below it.
  // macOS draws a native select's menu over the button, the picked item on top of it (owner
  // 2026-11-01: "the drop down sits over the drop down menu button"), so a click or an opening key
  // shows Wind's own list instead. The select stays the value holder and the combobox for screen
  // readers and tests; the colours come from the tokens.
  import { tick } from 'svelte';
  let { value = '', options = [], labels = null, onChange = () => {}, disabled = false,
        labelledby, describedby } = $props();
  // The page may hold a change back (a confirm notice, #441), leaving `value` as it was: draw the
  // select from the prop again after each change, so it never shows a value the page did not take.
  let sel = $state();
  let list = $state();
  let open = $state(false);
  let active = $state(0);
  let pos = $state({ left: 0, top: 0, width: 0, up: false });
  const uid = `sel-${Math.random().toString(36).slice(2, 9)}`;
  function pick(v) {
    onChange(v);
    tick().then(() => { if (sel) sel.value = String(value); });
  }
  const show = (o) => (labels && labels[o]) || o;

  // Fixed to the window, so a card's or the page's clipping never cuts it; above the select when
  // there is no room below.
  function place() {
    const r = sel.getBoundingClientRect();
    const h = options.length * 30 + 10;
    const up = r.bottom + 4 + h > window.innerHeight && r.top - 4 - h > 0;
    pos = { left: r.left, top: up ? r.top - 4 - h : r.bottom + 4, width: r.width, up };
  }
  async function openList() {
    if (disabled || open) return;
    active = Math.max(0, options.indexOf(String(value)));
    place();
    open = true;
    await tick();
    list?.focus({ preventScroll: true });
  }
  function close(refocus = true) {
    if (!open) return;
    open = false;
    if (refocus) sel?.focus({ preventScroll: true });
  }
  function choose(i) {
    const v = options[i];
    close();
    if (v !== undefined && v !== String(value)) pick(v);
  }

  function onSelectDown(e) {
    if (e.button !== 0) return;
    e.preventDefault();                  // no native menu
    if (open) close(); else openList();
  }
  function onSelectKey(e) {
    if ([' ', 'Enter', 'ArrowDown', 'ArrowUp'].includes(e.key) && !e.metaKey && !e.ctrlKey) {
      e.preventDefault();
      openList();
    }
  }
  function onListKey(e) {
    const n = options.length;
    switch (e.key) {
      case 'ArrowDown': active = (active + 1) % n; break;
      case 'ArrowUp': active = (active - 1 + n) % n; break;
      case 'Home': active = 0; break;
      case 'End': active = n - 1; break;
      case 'Enter': case ' ': choose(active); break;
      case 'Escape': close(); break;
      case 'Tab': close(false); return;    // let focus move on
      default: {
        // Type to jump: the next option starting with the letter.
        if (e.key.length !== 1) return;
        const k = e.key.toLowerCase();
        for (let j = 1; j <= n; j++) {
          const i = (active + j) % n;
          if (String(show(options[i])).toLowerCase().startsWith(k)) { active = i; break; }
        }
      }
    }
    e.preventDefault();
    e.stopPropagation();
  }
  // Any press outside, a scroll or a resize closes it (it would no longer sit under the select).
  $effect(() => {
    if (!open) return;
    const down = (e) => { if (!list?.contains(e.target) && e.target !== sel) close(false); };
    const away = (e) => { if (!list?.contains(e.target)) close(false); };
    window.addEventListener('pointerdown', down, true);
    window.addEventListener('scroll', away, true);
    window.addEventListener('resize', away);
    window.addEventListener('blur', away);
    return () => {
      window.removeEventListener('pointerdown', down, true);
      window.removeEventListener('scroll', away, true);
      window.removeEventListener('resize', away);
      window.removeEventListener('blur', away);
    };
  });
</script>

<span class="sel" class:disabled class:open>
  <select bind:this={sel} {disabled} value={String(value)} aria-labelledby={labelledby} aria-describedby={describedby}
          aria-expanded={open} aria-controls={open ? uid : undefined}
          onmousedown={onSelectDown} onkeydown={onSelectKey}
          onchange={(e) => pick(e.currentTarget.value)}>
    {#each options as o (o)}<option value={o}>{show(o)}</option>{/each}
  </select>
  <svg class="chev" viewBox="0 0 10 10" width="10" height="10" aria-hidden="true" focusable="false">
    <path d="M2 3.5 5 6.5 8 3.5" fill="none" stroke="currentColor" stroke-width="1.4"/>
  </svg>
</span>

{#if open}
  <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
  <ul class="list" class:up={pos.up} id={uid} role="listbox" tabindex="-1" bind:this={list}
      aria-labelledby={labelledby} aria-activedescendant={`${uid}-${active}`}
      style="left:{pos.left}px; top:{pos.top}px; min-width:{pos.width}px"
      onkeydown={onListKey} onblur={(e) => { if (!list?.contains(e.relatedTarget)) close(false); }}>
    {#each options as o, i (o)}
      <!-- svelte-ignore a11y_click_events_have_key_events -->
      <li id={`${uid}-${i}`} role="option" aria-selected={o === String(value)} class:active={i === active}
          onpointerenter={() => (active = i)} onclick={() => choose(i)}>
        <svg class="tick" viewBox="0 0 12 12" width="12" height="12" aria-hidden="true" focusable="false">
          {#if o === String(value)}<path d="M2.5 6.3 5 8.6 9.6 3.6" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>{/if}
        </svg>
        <span>{show(o)}</span>
      </li>
    {/each}
  </ul>
{/if}

<style>
  .sel { position: relative; display: inline-flex; align-items: center; color: var(--fg3); }
  .sel.disabled { opacity: .45; }
  select { -webkit-appearance: none; appearance: none; height: 30px; min-width: 150px;
           padding: 0 30px 0 12px; border-radius: var(--srad); border: 1px solid var(--chipb);
           background: var(--chip); color: var(--fg); font: 13px var(--s); cursor: pointer; }
  select:hover, .open select { border-color: var(--outline); }
  select:focus-visible { outline: 2px solid var(--fg); outline-offset: 2px; }
  select option { background: var(--card); color: var(--fg); }
  .chev { position: absolute; right: 11px; pointer-events: none; transition: transform var(--dur) var(--ease); }
  .open .chev { transform: rotate(180deg); }
  .list { position: fixed; z-index: 50; margin: 0; padding: 5px; list-style: none; box-sizing: border-box;
          border-radius: var(--srad); border: 1px solid var(--chipb); background: var(--card);
          box-shadow: var(--shadow); outline: none; animation: drop var(--dur) var(--ease); }
  .list.up { animation-name: rise; }
  li { display: flex; align-items: center; gap: 8px; height: 30px; padding: 0 12px 0 8px; border-radius: calc(var(--srad) - 2px);
       font: 13px var(--s); color: var(--fg); cursor: pointer; white-space: nowrap; }
  li.active { background: var(--hover2); }
  .tick { flex: none; color: var(--fg); }
  @keyframes drop { from { opacity: 0; transform: translateY(-4px); } }
  @keyframes rise { from { opacity: 0; transform: translateY(4px); } }
  @media (prefers-reduced-motion: reduce) { .list { animation: none; } .chev { transition: none; } }
</style>

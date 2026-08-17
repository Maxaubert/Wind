<script>
  // Collapsed listbox (the ARIA "select-only combobox" pattern). Keyboard support added for
  // issue #201: this used to be openable with Enter and then completely dead - arrows did nothing,
  // focus never entered the menu, and the options were <button role="option">, which is an invalid
  // pairing (an option is not a command). Focus now stays on the trigger the whole time and the
  // active option is published with aria-activedescendant, which is what lets a screen reader read
  // each option as you arrow through it without the focus ever leaving the control.
  export let value, options, onChange, disabled = false, labels = null;
  // Naming comes from the owning Row: `labelledby` lists the row label AND the value span below, so
  // the control reads "Magnifier model, Render" rather than just one half.
  export let labelledby = undefined, describedby = undefined, valueId = undefined;
  const show = (o) => (labels && labels[o]) ? labels[o] : o;
  let open = false;
  let el, triggerEl;
  let activeIdx = 0;                 // option under the keyboard cursor (NOT the committed value)
  // Only paint the keyboard cursor once a key has actually been used. Opening with the mouse would
  // otherwise draw a ring on the current option that means nothing to a pointer user.
  let kbNav = false;
  // Unique per instance: aria-activedescendant is an id reference, and several selects coexist.
  const uid = 'ls-' + Math.random().toString(36).slice(2, 8);
  const optId = (i) => `${uid}-o${i}`;

  function openMenu(idx) {
    if (disabled) return;
    // Start on the current value so arrowing moves from where you are, not from the top.
    const cur = options.indexOf(value);
    activeIdx = idx !== undefined ? idx : (cur >= 0 ? cur : 0);
    open = true;
  }
  function closeMenu(refocus = true) {
    open = false;
    if (refocus && triggerEl) triggerEl.focus();
  }
  // Only reachable by pointer: Enter/Space are preventDefault-ed in onKey, so they never
  // synthesise a click on the trigger.
  function toggle() { if (disabled) return; kbNav = false; open ? closeMenu() : openMenu(); }
  function pick(o) { onChange(o); closeMenu(); }
  function onWindowClick(e) { if (open && el && !el.contains(e.target)) open = false; }

  function move(delta) {
    const n = options.length;
    if (!n) return;
    activeIdx = (activeIdx + delta + n) % n;
  }
  // Type-ahead, like a native <select>: typing "r" jumps to Render. Buffer resets after a pause.
  let typed = '', typedAt = 0;
  function typeAhead(ch) {
    const now = performance.now();
    typed = (now - typedAt < 900 ? typed : '') + ch.toLowerCase();
    typedAt = now;
    const hit = options.findIndex(o => String(show(o)).toLowerCase().startsWith(typed));
    if (hit >= 0) { if (!open) openMenu(hit); else activeIdx = hit; }
  }
  function onKey(e) {
    if (disabled) return;
    kbNav = true;
    const k = e.key;
    if (!open) {
      if (k === 'ArrowDown' || k === 'ArrowUp' || k === 'Enter' || k === ' ') {
        e.preventDefault(); openMenu();
      } else if (k.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
        e.preventDefault(); typeAhead(k);
      }
      return;
    }
    if (k === 'ArrowDown')      { e.preventDefault(); move(1); }
    else if (k === 'ArrowUp')   { e.preventDefault(); move(-1); }
    else if (k === 'Home')      { e.preventDefault(); activeIdx = 0; }
    else if (k === 'End')       { e.preventDefault(); activeIdx = options.length - 1; }
    else if (k === 'Enter' || k === ' ') { e.preventDefault(); pick(options[activeIdx]); }
    else if (k === 'Escape')    { e.preventDefault(); e.stopPropagation(); closeMenu(); }
    // Tab commits nothing and closes, matching a native select's behaviour when it loses focus.
    else if (k === 'Tab')       { open = false; }
    else if (k.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) { e.preventDefault(); typeAhead(k); }
  }
</script>
<svelte:window on:click={onWindowClick} />
<div class="select" bind:this={el}>
  <!-- role="combobox" (the ARIA 1.2 select-only combobox): `button` does not support
       aria-activedescendant, and without it the active option is not published as you arrow. It is
       still a real <button>, so Enter/Space activation and disabled semantics come for free. -->
  <button class="trigger" type="button" role="combobox" {disabled} bind:this={triggerEl}
          aria-haspopup="listbox" aria-expanded={open}
          aria-controls={open ? uid : undefined}
          aria-activedescendant={open ? optId(activeIdx) : undefined}
          aria-labelledby={labelledby} aria-describedby={describedby}
          on:click|stopPropagation={toggle} on:keydown={onKey}>
    <span id={valueId}>{show(value)}</span>
    <svg viewBox="0 0 10 6" width="9" height="6" fill="none" stroke="currentColor" aria-hidden="true"
         focusable="false" stroke-width="1.3" stroke-linecap="round"><path d="M1 1l4 4 4-4"/></svg>
  </button>
  {#if open}
    <!-- Options are divs, not buttons: role="option" is a selectable item, and focus stays on the
         trigger (aria-activedescendant), so they must not be tab stops. Keyboard handling lives on
         the trigger above, which is why the click handler here needs no key handler of its own. -->
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <div class="menu" role="listbox" id={uid} aria-labelledby={labelledby}>
      {#each options as o, i}
        <div class="opt" class:selected={o === value} class:active={kbNav && i === activeIdx}
             id={optId(i)} role="option" aria-selected={o === value} tabindex="-1"
             on:mouseenter={() => (activeIdx = i)}
             on:click|stopPropagation={() => pick(o)}>{show(o)}</div>
      {/each}
    </div>
  {/if}
</div>
<style>
  .select { position: relative; display: inline-block; }
  .trigger { display: inline-flex; align-items: center; gap: 10px; padding: 5px 10px;
             background: var(--chip); color: var(--text); border: 1px solid var(--line);
             border-radius: 7px; font-size: 12px; cursor: pointer; min-width: 110px;
             justify-content: space-between; font-family: inherit; }
  .trigger:hover { background: var(--hover); }
  .trigger:disabled { opacity: .5; cursor: default; }
  .menu { position: absolute; top: calc(100% + 4px); right: 0; min-width: 130px; padding: 4px;
          background: var(--bg); color: var(--text); border: 1px solid var(--line);
          border-radius: 8px; box-shadow: 0 12px 30px rgba(0,0,0,.45);
          z-index: 50; display: flex; flex-direction: column; gap: 1px; }
  .opt { background: transparent; border: 0; color: var(--text); padding: 7px 11px;
         text-align: left; border-radius: 5px; cursor: pointer; font-size: 12.5px; font-family: inherit; }
  .opt:hover { background: var(--hover); }
  .opt.selected { background: var(--accent-soft); color: var(--accent-icon); }
  /* The keyboard cursor. Focus never leaves the trigger, so without this the arrow keys would move
     an invisible cursor - visible parity with what aria-activedescendant announces. */
  .opt.active { box-shadow: inset 0 0 0 2px var(--focus); }
</style>

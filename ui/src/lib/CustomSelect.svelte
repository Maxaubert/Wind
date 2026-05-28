<script>
  export let value, options, onChange, disabled = false;
  let open = false;
  let el;
  function toggle() { if (!disabled) open = !open; }
  function pick(o) { onChange(o); open = false; }
  function onWindowClick(e) { if (open && el && !el.contains(e.target)) open = false; }
  function onKey(e) { if (open && e.key === 'Escape') { open = false; e.preventDefault(); } }
</script>
<svelte:window on:click={onWindowClick} on:keydown={onKey} />
<div class="select" bind:this={el}>
  <button class="trigger" type="button" {disabled}
          aria-haspopup="listbox" aria-expanded={open}
          on:click|stopPropagation={toggle}>
    <span>{value}</span>
    <svg viewBox="0 0 10 6" width="9" height="6" fill="none" stroke="currentColor"
         stroke-width="1.3" stroke-linecap="round"><path d="M1 1l4 4 4-4"/></svg>
  </button>
  {#if open}
    <div class="menu" role="listbox">
      {#each options as o}
        <button class="opt" class:selected={o === value} role="option"
                aria-selected={o === value} type="button"
                on:click|stopPropagation={() => pick(o)}>{o}</button>
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
</style>

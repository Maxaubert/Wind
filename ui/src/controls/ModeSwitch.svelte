<script>
  // Mode: System / Light / Dark as one segmented radio group (arrows move, Tab leaves as one stop).
  // The value is the uiTheme ini value: auto / light / dark. Three equal-width segments; the selected one
  // is filled with the theme's selection colour and bold (ia07 mockup).
  let { value = 'auto', onChange = () => {}, labelledby, describedby } = $props();
  const modes = [['auto', 'System'], ['light', 'Light'], ['dark', 'Dark']];
  let el;
  function key(e) {
    const i = Math.max(0, modes.findIndex((m) => m[0] === value));
    let n = null;
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') n = (i + 1) % 3;
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') n = (i + 2) % 3;
    if (n === null) return;
    e.preventDefault();
    onChange(modes[n][0]);
    setTimeout(() => el && el.querySelectorAll('button')[n]?.focus(), 0);
  }
</script>

<div class="seg" bind:this={el} role="radiogroup" tabindex="-1" aria-labelledby={labelledby}
     aria-describedby={describedby} onkeydown={key}>
  {#each modes as [id, label] (id)}
    <button type="button" role="radio" aria-checked={value === id} tabindex={value === id ? 0 : -1}
            class:on={value === id} onclick={() => onChange(id)}>{label}</button>
  {/each}
</div>

<style>
  .seg { display: inline-flex; border: 1px solid var(--chipb); border-radius: var(--rad); overflow: hidden; }
  button { height: 28px; width: 72px; padding: 0; text-align: center; background: var(--chip); color: var(--fg3);
           font: 12px var(--s); }
  button + button { border-left: 1px solid var(--chipb); }
  button:hover:not(.on) { color: var(--fg); }
  button.on { background: var(--sel); color: var(--selfg); font-weight: 600; }
  button:focus-visible { outline-offset: -2px; }
</style>
